/*
 * fbe_helper: /data decryption support for the liuqin recovery.
 *
 *   fbe_helper start-tee        mount system/vendor/odm/persist/metadata read-only,
 *                               fix TEE device-node permissions, start qseecomd +
 *                               the QTI KeyMint HAL from the real vendor partition,
 *                               negotiate the KeyMint shared secret.
 *   fbe_helper mount-data [rw]  decrypt the metadata-encryption key with KeyMint,
 *                               set up dm-default-key "userdata" and mount /data
 *                               (read-only unless "rw"; rw is refused while an OTA
 *                               checkpoint is pending).
 *   fbe_helper unlock-de        install the system DE key and every user DE key, so
 *                               device-encrypted files show their real names.
 *   fbe_helper wipe-dalvik      delete everything in /data/dalvik-cache (needs rw mount).
 *   fbe_helper umount-data      unmount /data and remove the dm device.
 *   fbe_helper status
 *
 * Mirrors system/vold (KeyStorage::retrieveKey, MetadataCrypt, KeyUtil::installKey)
 * for keys with empty authentication. Never upgrades or rewrites key blobs, never
 * prints key material. Requires the dynamic partitions to be mapped already
 * (recovery maps them when it mounts any of them).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <aidl/android/hardware/security/keymint/ErrorCode.h>
#include <aidl/android/hardware/security/keymint/HardwareAuthToken.h>
#include <aidl/android/hardware/security/keymint/IKeyMintDevice.h>
#include <aidl/android/hardware/security/sharedsecret/ISharedSecret.h>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <fstab/fstab.h>
#include <libdm/dm.h>
#include <android/hardware/gatekeeper/1.0/IGatekeeper.h>
#include <arpa/inet.h>
#include <endian.h>
#include <openssl/aead.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <iostream>

using namespace aidl::android::hardware::security::keymint;
using aidl::android::hardware::security::sharedsecret::ISharedSecret;
using aidl::android::hardware::security::sharedsecret::SharedSecretParameters;
using android::base::GetProperty;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetDefaultKey;
using android::fs_mgr::Fstab;
using android::fs_mgr::FstabEntry;

namespace {

constexpr const char* kVendorFstab = "/vendor/etc/fstab.qcom";
constexpr const char* kLdPath =
    "/system/lib64:/vendor/lib64:/vendor/lib64/hw:"
    "/mnt/system/system/lib64/bootstrap:/mnt/system/system/lib64";
constexpr const char* kQseecomd = "/vendor/bin/qseecomd";
constexpr const char* kKeyMintBin = "/vendor/bin/hw/android.hardware.security.keymint-service-qti";
constexpr const char* kKeyMintSvc = "android.hardware.security.keymint.IKeyMintDevice/default";
constexpr const char* kSharedSecretSvc =
    "android.hardware.security.sharedsecret.ISharedSecret/default";
constexpr const char* kVintfSrc =
    "/vendor/etc/vintf/manifest/android.hardware.security.keymint-service-qti.xml";
constexpr const char* kVintfDst = "/system/etc/vintf/manifest/keymint-recovery.xml";
constexpr const char* kSharedSecretDone = "/tmp/.fbe_shared_secret";
constexpr const char* kDmName = "userdata";
constexpr const char* kHashPrefixSecdiscardable = "Android secdiscardable SHA512";
constexpr size_t kGcmNonceBytes = 12;
constexpr size_t kGcmMacBytes = 16;
constexpr uid_t kAidSystem = 1000;
constexpr gid_t kAidDrmrpc = 1026;

int Error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("ERROR: ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
  fflush(stdout);
  return 1;
}

void Info(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
  fflush(stdout);
}

// Zeroing byte buffer for secrets.
struct Secret {
  std::vector<uint8_t> b;
  ~Secret() { std::fill(b.begin(), b.end(), 0); }
};

bool Exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

bool IsMounted(const std::string& mp) {
  std::string mounts;
  if (!android::base::ReadFileToString("/proc/self/mounts", &mounts)) return false;
  for (const auto& line : android::base::Split(mounts, "\n")) {
    auto f = android::base::Split(line, " ");
    if (f.size() > 1 && f[1] == mp) return true;
  }
  return false;
}

bool MkdirP(const std::string& path) {
  std::string cur;
  for (const auto& part : android::base::Split(path, "/")) {
    if (part.empty()) continue;
    cur += "/" + part;
    if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return true;
}

bool MountRo(const std::string& dev, const std::string& mp, const std::vector<const char*>& types,
             const char* data) {
  if (IsMounted(mp)) return true;
  if (!Exists(dev)) {
    Error("%s does not exist", dev.c_str());
    return false;
  }
  MkdirP(mp);
  for (const char* t : types) {
    if (mount(dev.c_str(), mp.c_str(), t, MS_RDONLY | MS_NOSUID | MS_NODEV, data) == 0) {
      Info("mounted %s (%s, ro) at %s", dev.c_str(), t, mp.c_str());
      return true;
    }
  }
  Error("cannot mount %s at %s: %s", dev.c_str(), mp.c_str(), strerror(errno));
  return false;
}

bool PrepareMounts() {
  const std::string slot = GetProperty("ro.boot.slot_suffix", "");
  struct { const char* name; const char* mp; } dyn[] = {
    { "system", "/mnt/system" }, { "vendor", "/vendor" }, { "odm", "/odm" },
  };
  for (const auto& d : dyn) {
    std::string dev = std::string("/dev/block/mapper/") + d.name + slot;
    if (!Exists(dev)) {
      return Error("%s not mapped; open Tools > Mount partitions once first", dev.c_str()) == 0;
    }
    if (!MountRo(dev, d.mp, { "erofs", "ext4" }, nullptr)) return false;
  }
  if (!MountRo("/dev/block/by-name/persist", "/mnt/vendor/persist", { "ext4" }, "noload")) {
    return false;
  }
  if (!MountRo("/dev/block/by-name/metadata", "/metadata", { "ext4" }, "noload")) return false;
  return true;
}

// Same owners/modes as /vendor/etc/ueventd.rc; qseecomd drops to uid system
// before registering its listeners (RPMB, file system, ...).
void FixDeviceNodes() {
  auto set = [](const std::string& p, uid_t u, gid_t g, mode_t m) {
    if (chown(p.c_str(), u, g) != 0 || chmod(p.c_str(), m) != 0) {
      Info("warning: cannot set permissions on %s: %s", p.c_str(), strerror(errno));
    }
  };
  set("/dev/qseecom", kAidSystem, kAidDrmrpc, 0660);
  set("/dev/smcinvoke", kAidSystem, kAidDrmrpc, 0660);
  set("/dev/0:0:0:49476", kAidSystem, kAidSystem, 0600);
  if (std::unique_ptr<DIR, decltype(&closedir)> d(opendir("/dev/dma_heap"), closedir); d) {
    while (dirent* de = readdir(d.get())) {
      if (android::base::StartsWith(de->d_name, "qcom,")) {
        set(std::string("/dev/dma_heap/") + de->d_name, kAidSystem, kAidSystem, 0444);
      }
    }
  }
}

// Recovery images built with keymint-recovery.xml already declare the HALs.
// Otherwise copy the vendor fragment and restart servicemanager (and the two
// HALs that were registered with it) so it re-reads VINTF.
void EnsureVintf() {
  if (Exists(kVintfDst)) return;
  std::string xml;
  if (!android::base::ReadFileToString(kVintfSrc, &xml) ||
      !android::base::WriteStringToFile(xml, kVintfDst)) {
    Info("warning: cannot install %s", kVintfDst);
    return;
  }
  android::base::SetProperty("ctl.restart", "servicemanager");
  sleep(1);
  android::base::SetProperty("ctl.restart", "vendor.health-recovery");
  android::base::SetProperty("ctl.restart", "vendor.boot-qti");
  sleep(1);
}

bool ProcessRunning(const std::string& exe) {
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir("/proc"), closedir);
  if (!d) return false;
  while (dirent* de = readdir(d.get())) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    std::string cmd;
    if (!android::base::ReadFileToString(std::string("/proc/") + de->d_name + "/cmdline", &cmd)) {
      continue;
    }
    if (cmd.c_str() == exe) return true;
  }
  return false;
}

// Double-fork a daemon with the vendor library path; it outlives this helper.
void Spawn(const char* exe, const char* log) {
  pid_t pid = fork();
  if (pid == 0) {
    if (fork() != 0) _exit(0);
    setsid();
    int in = open("/dev/null", O_RDONLY);
    int out = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (in >= 0) dup2(in, 0);
    if (out >= 0) {
      dup2(out, 1);
      dup2(out, 2);
    }
    setenv("LD_LIBRARY_PATH", kLdPath, 1);
    execl(exe, exe, nullptr);
    _exit(127);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
}

template <typename T>
std::shared_ptr<T> WaitService(const char* name, int seconds) {
  for (int i = 0; i < seconds * 10; ++i) {
    ndk::SpAIBinder binder(AServiceManager_checkService(name));
    if (binder.get()) {
      if (auto s = T::fromBinder(binder)) return s;
    }
    usleep(100 * 1000);
  }
  return nullptr;
}

std::string StatusText(const ndk::ScopedAStatus& st) {
  if (st.getExceptionCode() == EX_SERVICE_SPECIFIC) {
    int code = st.getServiceSpecificError();
    std::string s = "KeyMint error " + std::to_string(code);
    if (code == static_cast<int>(ErrorCode::KEY_REQUIRES_UPGRADE)) {
      s += " (KEY_REQUIRES_UPGRADE: refusing to upgrade keys from recovery)";
    }
    return s;
  }
  return st.getDescription();
}

int StartTee() {
  if (!PrepareMounts()) return 1;
  FixDeviceNodes();
  EnsureVintf();
  if (!ProcessRunning(kQseecomd)) {
    Spawn(kQseecomd, "/tmp/fbe-qseecomd.log");
    for (int i = 0; i < 100; ++i) {
      if (GetProperty("vendor.sys.listeners.registered", "") == "true") break;
      usleep(100 * 1000);
    }
  }
  if (GetProperty("vendor.sys.listeners.registered", "") != "true") {
    return Error("qseecomd listeners not registered (see /tmp/fbe-qseecomd.log)");
  }
  if (!ProcessRunning(kKeyMintBin)) Spawn(kKeyMintBin, "/tmp/fbe-keymint.log");
  auto km = WaitService<IKeyMintDevice>(kKeyMintSvc, 15);
  if (!km) return Error("KeyMint HAL did not come up (see /tmp/fbe-keymint.log)");

  if (!Exists(kSharedSecretDone)) {
    auto ss = WaitService<ISharedSecret>(kSharedSecretSvc, 5);
    if (!ss) return Error("ISharedSecret not available");
    SharedSecretParameters params;
    std::vector<uint8_t> check;
    auto st = ss->getSharedSecretParameters(&params);
    if (st.isOk()) st = ss->computeSharedSecret({ params }, &check);
    if (!st.isOk()) return Error("shared secret negotiation failed: %s", StatusText(st).c_str());
    android::base::WriteStringToFile("1", kSharedSecretDone);
  }
  KeyMintHardwareInfo info;
  km->getHardwareInfo(&info);
  Info("TEE ready: %s (KeyMint v%d)", info.keyMintName.c_str(), info.versionNumber);
  return 0;
}

std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::string HashWithPrefix(const char* prefix, const std::string& data) {
  SHA512_CTX c;
  SHA512_Init(&c);
  std::string p = prefix;
  p.resize(SHA512_CBLOCK);
  SHA512_Update(&c, p.data(), p.size());
  SHA512_Update(&c, data.data(), data.size());
  std::string res(SHA512_DIGEST_LENGTH, '\0');
  SHA512_Final(reinterpret_cast<uint8_t*>(res.data()), &c);
  return res;
}

// vold KeyStorage::retrieveKey() for a key with empty authentication.
bool RetrieveKey(IKeyMintDevice& km, const std::string& dir, Secret* key) {
  std::string version, secdiscardable, encrypted, blob;
  if (!android::base::ReadFileToString(dir + "/version", &version) ||
      !android::base::ReadFileToString(dir + "/secdiscardable", &secdiscardable) ||
      !android::base::ReadFileToString(dir + "/encrypted_key", &encrypted) ||
      !android::base::ReadFileToString(dir + "/keymaster_key_blob", &blob)) {
    return Error("cannot read key directory %s: %s", dir.c_str(), strerror(errno)) == 0;
  }
  if (Exists(dir + "/keymaster_key_blob_upgraded")) {
    return Error("%s has a pending key upgrade; refusing (boot Android once first)",
                 dir.c_str()) == 0;
  }
  if (version != "1" || encrypted.size() < kGcmNonceBytes + kGcmMacBytes) {
    return Error("unexpected key format in %s", dir.c_str()) == 0;
  }
  const std::string app_id = HashWithPrefix(kHashPrefixSecdiscardable, secdiscardable);
  std::vector<KeyParameter> params = {
    { Tag::BLOCK_MODE, KeyParameterValue::make<KeyParameterValue::blockMode>(BlockMode::GCM) },
    { Tag::PADDING, KeyParameterValue::make<KeyParameterValue::paddingMode>(PaddingMode::NONE) },
    { Tag::MAC_LENGTH, KeyParameterValue::make<KeyParameterValue::integer>(kGcmMacBytes * 8) },
    { Tag::APPLICATION_ID, KeyParameterValue::make<KeyParameterValue::blob>(Bytes(app_id)) },
    { Tag::NONCE,
      KeyParameterValue::make<KeyParameterValue::blob>(Bytes(encrypted.substr(0, kGcmNonceBytes))) },
  };
  BeginResult begin;
  auto st = km.begin(KeyPurpose::DECRYPT, Bytes(blob), params, std::nullopt, &begin);
  if (!st.isOk() || !begin.operation) {
    return Error("KeyMint begin for %s: %s", dir.c_str(), StatusText(st).c_str()) == 0;
  }
  st = begin.operation->finish(Bytes(encrypted.substr(kGcmNonceBytes)), std::nullopt, std::nullopt,
                               std::nullopt, std::nullopt, &key->b);
  if (!st.isOk()) {
    return Error("KeyMint finish for %s: %s", dir.c_str(), StatusText(st).c_str()) == 0;
  }
  return true;
}

// Hardware-wrapped (wrappedkey_v0) long-term key -> per-boot ephemeral key.
bool ToEphemeral(IKeyMintDevice& km, const Secret& lt, Secret* eph) {
  auto st = km.convertStorageKeyToEphemeral(lt.b, &eph->b);
  if (!st.isOk()) return Error("convertStorageKeyToEphemeral: %s", StatusText(st).c_str()) == 0;
  return true;
}

std::shared_ptr<IKeyMintDevice> KeyMintOrError() {
  auto km = WaitService<IKeyMintDevice>(kKeyMintSvc, 3);
  if (!km || !Exists(kSharedSecretDone)) Error("TEE not started; run start-tee first");
  return km;
}

const FstabEntry* DataEntry(Fstab* fstab) {
  if (!android::fs_mgr::ReadFstabFromFile(kVendorFstab, fstab)) {
    Error("cannot read %s (is /vendor mounted?)", kVendorFstab);
    return nullptr;
  }
  const FstabEntry* e = android::fs_mgr::GetEntryForMountPoint(fstab, "/data");
  if (!e) Error("no /data entry in %s", kVendorFstab);
  return e;
}

int MountData(bool rw) {
  if (IsMounted("/data")) {
    Info("/data is already mounted");
    return 0;
  }
  Fstab fstab;
  const FstabEntry* e = DataEntry(&fstab);
  if (!e) return 1;
  if (e->metadata_key_dir.empty() || e->metadata_encryption_options.empty()) {
    return Error("/data has no metadata encryption options in fstab");
  }
  auto opts = android::base::Split(e->metadata_encryption_options, ":");
  if (opts[0] != "aes-256-xts" || opts.size() != 2 || opts[1] != "wrappedkey_v0") {
    return Error("unsupported metadata encryption options: %s",
                 e->metadata_encryption_options.c_str());
  }
  if (rw && Exists("/metadata/vold/checkpoint")) {
    return Error("an OTA checkpoint is pending (/metadata/vold/checkpoint); refusing rw mount. "
                 "Boot Android once so the update is committed, or mount read-only.");
  }
  auto km = KeyMintOrError();
  if (!km) return 1;

  std::string key_dir = e->metadata_key_dir + "/key";
  Secret lt, eph;
  if (!RetrieveKey(*km, key_dir, &lt) || !ToEphemeral(*km, lt, &eph)) return 1;

  unique_fd blk(open(e->blk_device.c_str(), O_RDONLY | O_CLOEXEC));
  uint64_t bytes = 0;
  if (blk == -1 || ioctl(blk.get(), BLKGETSIZE64, &bytes) != 0) {
    return Error("cannot size %s: %s", e->blk_device.c_str(), strerror(errno));
  }
  uint64_t nr_sec = (bytes / 512) & ~7ULL;

  std::string hex;
  hex.reserve(eph.b.size() * 2);
  static constexpr char kHex[] = "0123456789abcdef";
  for (uint8_t c : eph.b) {
    hex.push_back(kHex[c >> 4]);
    hex.push_back(kHex[c & 15]);
  }
  auto target = std::make_unique<DmTargetDefaultKey>(0, nr_sec, "aes-xts-plain64", hex,
                                                     e->blk_device, 0);
  target->SetSetDun();
  target->SetWrappedKeyV0();
  std::fill(hex.begin(), hex.end(), '0');
  DmTable table;
  table.AddTarget(std::move(target));

  auto& dm = DeviceMapper::Instance();
  if (dm.GetState(kDmName) != DmDeviceState::INVALID) dm.DeleteDevice(kDmName);
  std::string dm_path;
  if (!dm.CreateDevice(kDmName, table, &dm_path, std::chrono::seconds(5))) {
    return Error("cannot create dm-default-key device %s", kDmName);
  }

  std::string data = e->fs_options;
  unsigned long flags = e->flags;
  if (!rw) {
    flags |= MS_RDONLY;
    data += std::string(data.empty() ? "" : ",") + "disable_roll_forward";
  }
  MkdirP("/data");
  if (mount(dm_path.c_str(), "/data", e->fs_type.c_str(), flags, data.c_str()) != 0) {
    int err = errno;
    dm.DeleteDevice(kDmName);
    return Error("mount %s on /data (%s) failed: %s", dm_path.c_str(), data.c_str(), strerror(err));
  }
  Info("/data mounted %s (%s, metadata-decrypted)", rw ? "read-write" : "read-only",
       e->fs_type.c_str());
  return 0;
}

bool AddFscryptKey(const Secret& eph, std::string* identifier) {
  std::vector<uint8_t> buf(sizeof(fscrypt_add_key_arg) + eph.b.size(), 0);
  auto* arg = reinterpret_cast<fscrypt_add_key_arg*>(buf.data());
  arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
  arg->__flags |= __FSCRYPT_ADD_KEY_FLAG_HW_WRAPPED;  // wrappedkey_v0 (legacy Android flag)
  arg->raw_size = eph.b.size();
  memcpy(arg->raw, eph.b.data(), eph.b.size());
  unique_fd fd(open("/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  int r = fd == -1 ? -1 : ioctl(fd.get(), FS_IOC_ADD_ENCRYPTION_KEY, arg);
  int err = errno;
  std::string id;
  for (int i = 0; i < FSCRYPT_KEY_IDENTIFIER_SIZE; ++i) {
    char h[3];
    snprintf(h, sizeof(h), "%02x", arg->key_spec.u.identifier[i]);
    id += h;
  }
  std::fill(buf.begin(), buf.end(), 0);
  if (r != 0) return Error("FS_IOC_ADD_ENCRYPTION_KEY: %s", strerror(err)) == 0;
  *identifier = id;
  return true;
}

int InstallKeyDir(IKeyMintDevice& km, const std::string& dir, const char* what) {
  Secret lt, eph;
  std::string id;
  if (!RetrieveKey(km, dir, &lt) || !ToEphemeral(km, lt, &eph) || !AddFscryptKey(eph, &id)) {
    return 1;
  }
  Info("installed %s key (identifier %s)", what, id.c_str());
  return 0;
}

int UnlockDe() {
  if (!IsMounted("/data")) return Error("/data is not mounted");
  Fstab fstab;
  const FstabEntry* e = DataEntry(&fstab);
  if (!e) return 1;
  if (e->encryption_options.find("v2") == std::string::npos ||
      e->encryption_options.find("wrappedkey_v0") == std::string::npos) {
    return Error("unsupported file encryption options: %s", e->encryption_options.c_str());
  }
  auto km = KeyMintOrError();
  if (!km) return 1;
  if (InstallKeyDir(*km, "/data/unencrypted/key", "system DE")) return 1;

  const std::string de_root = "/data/misc/vold/user_keys/de";
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(de_root.c_str()), closedir);
  if (!d) return Error("cannot open %s: %s", de_root.c_str(), strerror(errno));
  int failed = 0;
  while (dirent* de = readdir(d.get())) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    std::string what = std::string("user ") + de->d_name + " DE";
    failed += InstallKeyDir(*km, de_root + "/" + de->d_name, what.c_str());
  }
  // Sanity check: device-encrypted directories now show plaintext names.
  int names = 0;
  if (std::unique_ptr<DIR, decltype(&closedir)> dc(opendir("/data/dalvik-cache"), closedir); dc) {
    while (dirent* de = readdir(dc.get())) {
      if (de->d_name[0] != '.') {
        if (names < 4) Info("  /data/dalvik-cache/%s", de->d_name);
        ++names;
      }
    }
  }
  Info("DE unlock %s (%d entries in /data/dalvik-cache)", failed ? "PARTIAL" : "done", names);
  return failed ? 1 : 0;
}

bool IsReadWrite(const std::string& mp) {
  std::string mounts;
  android::base::ReadFileToString("/proc/self/mounts", &mounts);
  for (const auto& line : android::base::Split(mounts, "\n")) {
    auto f = android::base::Split(line, " ");
    if (f.size() > 3 && f[1] == mp) return android::base::StartsWith(f[3], "rw");
  }
  return false;
}

int RemoveTree(const std::string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return errno == ENOENT ? 0 : 1;
  int failed = 0;
  if (S_ISDIR(st.st_mode)) {
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), closedir);
    if (d) {
      while (dirent* de = readdir(d.get())) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        failed += RemoveTree(path + "/" + de->d_name);
      }
    }
    d.reset();
    if (rmdir(path.c_str()) != 0) ++failed;
  } else if (unlink(path.c_str()) != 0) {
    ++failed;
  }
  return failed;
}

int WipeDalvik() {
  if (!IsMounted("/data") || !IsReadWrite("/data")) return Error("/data is not mounted read-write");
  const std::string dir = "/data/dalvik-cache";
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(dir.c_str()), closedir);
  if (!d) return Error("cannot open %s: %s", dir.c_str(), strerror(errno));
  int failed = 0, removed = 0;
  while (dirent* de = readdir(d.get())) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    // Keep the per-ABI directories themselves (init recreates them anyway).
    failed += RemoveTree(dir + "/" + de->d_name);
    ++removed;
  }
  sync();
  Info("dalvik-cache: removed %d entries%s", removed, failed ? " (some failed)" : "");
  return failed ? 1 : 0;
}

int UmountData() {
  if (IsMounted("/data") && umount("/data") != 0) {
    return Error("umount /data: %s", strerror(errno));
  }
  auto& dm = DeviceMapper::Instance();
  if (dm.GetState(kDmName) != DmDeviceState::INVALID && !dm.DeleteDevice(kDmName)) {
    return Error("cannot remove dm device %s", kDmName);
  }
  Info("/data unmounted");
  return 0;
}

int Status() {
  Info("qseecomd: %s, listeners: %s", ProcessRunning(kQseecomd) ? "running" : "stopped",
       GetProperty("vendor.sys.listeners.registered", "-").c_str());
  Info("keymint: %s, shared secret: %s", ProcessRunning(kKeyMintBin) ? "running" : "stopped",
       Exists(kSharedSecretDone) ? "yes" : "no");
  Info("/data: %s", IsMounted("/data") ? (IsReadWrite("/data") ? "mounted rw" : "mounted ro")
                                       : "not mounted");
  Info("checkpoint pending: %s", Exists("/metadata/vold/checkpoint") ? "yes" : "no");
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// unlock-ce: lock-screen credential -> synthetic password -> CE storage key.
// Mirrors frameworks/base SyntheticPasswordManager (LSKF protector, Gatekeeper,
// no Weaver, SP v3) and vold fscrypt_unlock_ce_storage(). Read-only: no
// Gatekeeper re-enroll, no key upgrades, no "fixation" of CE key directories.
// ---------------------------------------------------------------------------
namespace {

using android::hardware::hidl_vec;
using android::hardware::gatekeeper::V1_0::GatekeeperResponse;
using android::hardware::gatekeeper::V1_0::GatekeeperStatusCode;
using android::hardware::gatekeeper::V1_0::IGatekeeper;

constexpr int kSpVersionV3 = 3;
constexpr int kProtectorTypeLskf = 0;
constexpr int kDomainSelinux = 2;
constexpr int kNamespaceLocksettings = 103;
constexpr int kBlobMetaEncryptedBy = 0;
constexpr const char* kSpContext = "android-synthetic-password-personalization-context";
constexpr const char* kKeystoreDb = "file:/data/misc/keystore/persistent.sqlite?immutable=1";

struct __attribute__((packed)) HwAuthTokenRaw {  // hardware/hw_auth_token.h
  uint8_t version;
  uint64_t challenge;
  uint64_t user_id;
  uint64_t authenticator_id;
  uint32_t authenticator_type;  // network byte order
  uint64_t timestamp;           // network byte order
  uint8_t hmac[32];
};
static_assert(sizeof(HwAuthTokenRaw) == 69, "hw_auth_token_t layout");

void Zero(std::string* s) {
  std::fill(s->begin(), s->end(), '\0');
  s->clear();
}

// SyntheticPasswordCrypto.personalizedHash(): SHA-512(pad128(personalization) || data).
std::string PersonalizedHash(const std::string& personalization, const std::string& data) {
  SHA512_CTX c;
  SHA512_Init(&c);
  std::string p = personalization;
  p.resize(SHA512_CBLOCK, '\0');
  SHA512_Update(&c, p.data(), p.size());
  SHA512_Update(&c, data.data(), data.size());
  std::string out(SHA512_DIGEST_LENGTH, '\0');
  SHA512_Final(reinterpret_cast<uint8_t*>(out.data()), &c);
  return out;
}

// AES-256-GCM, input = nonce(12) || ciphertext || tag(16).
bool AesGcmOpen(const std::string& key, const std::string& in, std::string* out) {
  if (key.size() < 32 || in.size() < 12 + 16) return false;
  EVP_AEAD_CTX ctx;
  if (!EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), reinterpret_cast<const uint8_t*>(key.data()),
                         32, 16, nullptr)) {
    return false;
  }
  out->resize(in.size());
  size_t out_len = 0;
  bool ok = EVP_AEAD_CTX_open(&ctx, reinterpret_cast<uint8_t*>(out->data()), &out_len, out->size(),
                              reinterpret_cast<const uint8_t*>(in.data()), 12,
                              reinterpret_cast<const uint8_t*>(in.data()) + 12, in.size() - 12,
                              nullptr, 0);
  EVP_AEAD_CTX_cleanup(&ctx);
  out->resize(ok ? out_len : 0);
  return ok;
}

// SP800Derive.withContext() (HMAC-SHA256, counter 1, L = 256).
std::string Sp800Derive(const std::string& key, const std::string& label,
                        const std::string& context) {
  auto be32 = [](uint32_t v) {
    std::string s(4, '\0');
    s[0] = v >> 24; s[1] = v >> 16; s[2] = v >> 8; s[3] = v;
    return s;
  };
  std::string msg = be32(1) + label + std::string(1, '\0') + context +
                    be32(static_cast<uint32_t>(context.size() * 8)) + be32(256);
  std::string out(32, '\0');
  unsigned int len = 0;
  HMAC(EVP_sha256(), key.data(), key.size(), reinterpret_cast<const uint8_t*>(msg.data()),
       msg.size(), reinterpret_cast<uint8_t*>(out.data()), &len);
  return out;
}

struct PasswordData {
  int credential_type = 0;
  int log_n = 0, log_r = 0, log_p = 0;
  std::string salt;
  std::string handle;
};

bool ParsePasswordData(const std::string& d, PasswordData* pd) {
  size_t pos = 0;
  auto u32 = [&](uint32_t* v) {
    if (pos + 4 > d.size()) return false;
    *v = (uint8_t(d[pos]) << 24) | (uint8_t(d[pos + 1]) << 16) | (uint8_t(d[pos + 2]) << 8) |
         uint8_t(d[pos + 3]);
    pos += 4;
    return true;
  };
  uint32_t type, len;
  if (!u32(&type)) return false;
  pd->credential_type = static_cast<int16_t>(type & 0xffff);
  if (pos + 3 > d.size()) return false;
  pd->log_n = d[pos++];
  pd->log_r = d[pos++];
  pd->log_p = d[pos++];
  if (!u32(&len) || pos + len > d.size()) return false;
  pd->salt = d.substr(pos, len);
  pos += len;
  if (!u32(&len) || pos + len > d.size()) return false;
  pd->handle = d.substr(pos, len);
  return !pd->handle.empty();
}

// The LSKF protector of |user|: the *.pwd file in /data/system_de/<user>/spblob.
bool FindProtector(int user, std::string* id_hex) {
  std::string dir = "/data/system_de/" + std::to_string(user) + "/spblob";
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(dir.c_str()), closedir);
  if (!d) return Error("cannot open %s (DE storage locked?)", dir.c_str()) == 0;
  std::vector<std::string> ids;
  while (dirent* de = readdir(d.get())) {
    std::string n = de->d_name;
    if (android::base::EndsWith(n, ".pwd") && n.size() == 20) ids.push_back(n.substr(0, 16));
  }
  if (ids.size() != 1) {
    return Error("expected exactly one LSKF protector in %s, found %zu", dir.c_str(),
                 ids.size()) == 0;
  }
  *id_hex = ids[0];
  return true;
}

// keystore2: Domain::SELINUX / NAMESPACE_LOCKSETTINGS key blob (not super-encrypted).
bool KeystoreBlob(const std::string& alias, std::string* blob) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(kKeystoreDb, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) !=
      SQLITE_OK) {
    Error("cannot open keystore database: %s", db ? sqlite3_errmsg(db) : "?");
    sqlite3_close(db);
    return false;
  }
  bool ok = false;
  sqlite3_stmt* st = nullptr;
  const char* q =
      "SELECT b.id, b.blob FROM blobentry b JOIN keyentry k ON b.keyentryid = k.id "
      "WHERE k.domain = ?1 AND k.namespace = ?2 AND k.alias = ?3 AND k.state = 1 "
      "AND b.subcomponent_type = 0 ORDER BY b.id DESC LIMIT 1";
  int64_t blob_id = 0;
  if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(st, 1, kDomainSelinux);
    sqlite3_bind_int(st, 2, kNamespaceLocksettings);
    sqlite3_bind_text(st, 3, alias.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
      blob_id = sqlite3_column_int64(st, 0);
      const void* p = sqlite3_column_blob(st, 1);
      int n = sqlite3_column_bytes(st, 1);
      if (p && n > 0) {
        blob->assign(static_cast<const char*>(p), n);
        ok = true;
      }
    }
  }
  sqlite3_finalize(st);
  if (ok) {
    st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM blobmetadata WHERE blobentryid = ?1 AND tag = ?2",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, blob_id);
      sqlite3_bind_int(st, 2, kBlobMetaEncryptedBy);
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) != 0) {
        Error("keystore blob for %s is super-encrypted; not supported", alias.c_str());
        ok = false;
      }
    }
    sqlite3_finalize(st);
  } else {
    Error("keystore key %s not found", alias.c_str());
  }
  sqlite3_close(db);
  return ok;
}

// Gatekeeper (passthrough HAL, in-process). Returns the hw_auth_token on success.
int GatekeeperVerify(int user, const std::string& handle, const std::string& gk_password,
                     std::vector<uint8_t>* hat) {
  android::sp<IGatekeeper> gk = IGatekeeper::getService("default", true /* getStub */);
  if (gk == nullptr) return Error("Gatekeeper HAL not available");
  GatekeeperStatusCode code = GatekeeperStatusCode::ERROR_GENERAL_FAILURE;
  uint32_t timeout = 0;
  hidl_vec<uint8_t> h(handle.begin(), handle.end());
  hidl_vec<uint8_t> p(gk_password.begin(), gk_password.end());
  auto ret = gk->verify(100000 + user /* fakeUserId */, 0, h, p,
                        [&](const GatekeeperResponse& r) {
                          code = r.code;
                          timeout = r.timeout;
                          hat->assign(r.data.begin(), r.data.end());
                        });
  std::fill(p.begin(), p.end(), 0);
  if (!ret.isOk()) return Error("Gatekeeper verify transport error");
  if (code == GatekeeperStatusCode::ERROR_RETRY_TIMEOUT) {
    return Error("too many attempts; Gatekeeper asks to wait %u ms before retrying", timeout);
  }
  if (code != GatekeeperStatusCode::STATUS_OK && code != GatekeeperStatusCode::STATUS_REENROLL) {
    return Error("wrong PIN/password (Gatekeeper refused it)");
  }
  if (hat->size() != sizeof(HwAuthTokenRaw)) {
    return Error("unexpected auth token size %zu", hat->size());
  }
  return 0;
}

HardwareAuthToken ToHardwareAuthToken(const std::vector<uint8_t>& raw) {
  HwAuthTokenRaw t;
  memcpy(&t, raw.data(), sizeof(t));
  HardwareAuthToken h;
  h.challenge = static_cast<int64_t>(t.challenge);
  h.userId = static_cast<int64_t>(t.user_id);
  h.authenticatorId = static_cast<int64_t>(t.authenticator_id);
  h.authenticatorType = static_cast<HardwareAuthenticatorType>(ntohl(t.authenticator_type));
  h.timestamp.milliSeconds = static_cast<int64_t>(be64toh(t.timestamp));
  h.mac.assign(t.hmac, t.hmac + sizeof(t.hmac));
  return h;
}

int UnlockCe(int user) {
  if (!IsMounted("/data")) return Error("/data is not mounted (decrypt /data first)");
  auto km = KeyMintOrError();
  if (!km) return 1;

  std::string pid;
  if (!FindProtector(user, &pid)) return 1;
  const std::string base = "/data/system_de/" + std::to_string(user) + "/spblob/" + pid;
  std::string pwd_bytes, secdis, spblob;
  if (!android::base::ReadFileToString(base + ".pwd", &pwd_bytes) ||
      !android::base::ReadFileToString(base + ".secdis", &secdis) ||
      !android::base::ReadFileToString(base + ".spblob", &spblob)) {
    return Error("cannot read protector %s files", pid.c_str());
  }
  PasswordData pd;
  if (!ParsePasswordData(pwd_bytes, &pd)) return Error("cannot parse %s.pwd", pid.c_str());
  if (spblob.size() < 2 + 12 + 16 || spblob[0] != kSpVersionV3 || spblob[1] != kProtectorTypeLskf) {
    return Error("unsupported SP blob (version %d, type %d)", spblob.size() ? spblob[0] : -1,
                 spblob.size() > 1 ? spblob[1] : -1);
  }
  Info("protector %s: credential type %d, scrypt N=2^%d r=2^%d p=2^%d", pid.c_str(),
       pd.credential_type, pd.log_n, pd.log_r, pd.log_p);

  // The credential arrives on stdin (never on the command line).
  std::string pin;
  std::getline(std::cin, pin);
  if (!pin.empty() && pin.back() == '\r') pin.pop_back();
  if (pin.empty()) return Error("no credential given");

  // stretchLskf(): scrypt -> 32 bytes.
  std::string stretched(32, '\0');
  bool scrypt_ok = EVP_PBE_scrypt(pin.data(), pin.size(),
                                  reinterpret_cast<const uint8_t*>(pd.salt.data()), pd.salt.size(),
                                  1ULL << pd.log_n, 1ULL << pd.log_r, 1ULL << pd.log_p,
                                  64 * 1024 * 1024,
                                  reinterpret_cast<uint8_t*>(stretched.data()), 32);
  Zero(&pin);
  if (!scrypt_ok) return Error("scrypt failed");

  std::string gk_password = PersonalizedHash("user-gk-authentication", stretched);
  std::vector<uint8_t> hat;
  int rc = GatekeeperVerify(user, pd.handle, gk_password, &hat);
  Zero(&gk_password);
  if (rc != 0) {
    Zero(&stretched);
    return rc;
  }
  Info("Gatekeeper: credential accepted");

  // protectorSecret = stretchedLskf || personalizedHash("secdiscardable-transform", secdis)
  std::string protector_secret = stretched + PersonalizedHash("secdiscardable-transform", secdis);
  Zero(&stretched);

  // Outer layer: auth-bound Keystore (KeyMint) key "synthetic_password_<%x id>".
  char alias[64];
  snprintf(alias, sizeof(alias), "synthetic_password_%llx",
           static_cast<unsigned long long>(strtoull(pid.c_str(), nullptr, 16)));
  std::string key_blob;
  if (!KeystoreBlob(alias, &key_blob)) {
    Zero(&protector_secret);
    return 1;
  }
  const std::string content = spblob.substr(2);
  std::vector<KeyParameter> params = {
    { Tag::BLOCK_MODE, KeyParameterValue::make<KeyParameterValue::blockMode>(BlockMode::GCM) },
    { Tag::PADDING, KeyParameterValue::make<KeyParameterValue::paddingMode>(PaddingMode::NONE) },
    { Tag::MAC_LENGTH, KeyParameterValue::make<KeyParameterValue::integer>(128) },
    { Tag::NONCE, KeyParameterValue::make<KeyParameterValue::blob>(Bytes(content.substr(0, 12))) },
  };
  BeginResult begin;
  auto st = km->begin(KeyPurpose::DECRYPT, Bytes(key_blob), params, ToHardwareAuthToken(hat),
                      &begin);
  if (!st.isOk() || !begin.operation) {
    Zero(&protector_secret);
    return Error("KeyMint begin (SP protector key): %s", StatusText(st).c_str());
  }
  Secret intermediate;
  st = begin.operation->finish(Bytes(content.substr(12)), std::nullopt, ToHardwareAuthToken(hat),
                               std::nullopt, std::nullopt, &intermediate.b);
  if (!st.isOk()) {
    Zero(&protector_secret);
    return Error("KeyMint finish (SP protector key): %s", StatusText(st).c_str());
  }

  // Inner layer: AES-GCM with personalizedHash("application-id", protectorSecret)[0:32].
  std::string inner_key = PersonalizedHash("application-id", protector_secret);
  Zero(&protector_secret);
  std::string sp;
  bool inner_ok = AesGcmOpen(inner_key,
                             std::string(intermediate.b.begin(), intermediate.b.end()), &sp);
  Zero(&inner_key);
  if (!inner_ok) return Error("cannot decrypt synthetic password (inner layer)");

  // SP v3: FBE secret = SP800Derive(sp).withContext("fbe-key", context).
  std::string fbe_secret = Sp800Derive(sp, "fbe-key", kSpContext);
  Zero(&sp);
  Info("synthetic password unwrapped");

  // vold: CE key dirs (newest first), software-wrapped with appId = H(secdiscardable) || secret.
  const std::string ce_dir = "/data/misc/vold/user_keys/ce/" + std::to_string(user);
  std::vector<std::string> paths;
  if (std::unique_ptr<DIR, decltype(&closedir)> d(opendir(ce_dir.c_str()), closedir); d) {
    while (dirent* de = readdir(d.get())) {
      if (de->d_type == DT_DIR && de->d_name[0] == 'c') paths.push_back(ce_dir + "/" + de->d_name);
    }
  }
  std::sort(paths.rbegin(), paths.rend());
  Secret ce_key;
  for (const auto& path : paths) {
    std::string version, encrypted, secdisc, secdisc_hash;
    if (!android::base::ReadFileToString(path + "/version", &version) || version != "1" ||
        !android::base::ReadFileToString(path + "/encrypted_key", &encrypted)) {
      continue;
    }
    if (android::base::ReadFileToString(path + "/secdiscardable", &secdisc)) {
      secdisc_hash = HashWithPrefix(kHashPrefixSecdiscardable, secdisc);
    }
    std::string wrap_key = HashWithPrefix("Android key wrapping key generation SHA512",
                                          secdisc_hash + fbe_secret);
    std::string plain;
    bool ok = AesGcmOpen(wrap_key, encrypted, &plain);
    Zero(&wrap_key);
    if (ok) {
      ce_key.b.assign(plain.begin(), plain.end());
      Zero(&plain);
      Info("CE key decrypted from %s", path.c_str());
      break;
    }
  }
  Zero(&fbe_secret);
  if (ce_key.b.empty()) return Error("no CE key in %s could be decrypted", ce_dir.c_str());

  Secret eph;
  std::string id;
  if (!ToEphemeral(*km, ce_key, &eph) || !AddFscryptKey(eph, &id)) return 1;
  Info("installed user %d CE key (identifier %s)", user, id.c_str());

  int shown = 0;
  const std::string media = "/data/media/" + std::to_string(user);
  if (std::unique_ptr<DIR, decltype(&closedir)> d(opendir(media.c_str()), closedir); d) {
    while (dirent* de = readdir(d.get())) {
      if (de->d_name[0] == '.' || shown >= 8) continue;
      Info("  %s/%s", media.c_str(), de->d_name);
      ++shown;
    }
  }
  Info("CE unlock done: internal storage is at %s", media.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  ABinderProcess_startThreadPool();
  std::string cmd = argc > 1 ? argv[1] : "";
  if (cmd == "start-tee") return StartTee();
  if (cmd == "mount-data") return MountData(argc > 2 && std::string(argv[2]) == "rw");
  if (cmd == "unlock-de") return UnlockDe();
  if (cmd == "wipe-dalvik") return WipeDalvik();
  if (cmd == "umount-data") return UmountData();
  if (cmd == "status") return Status();
  if (cmd == "unlock-ce") {
    // The Gatekeeper HAL is dlopen()ed from /vendor; its dependencies need the vendor
    // library path, which the linker only reads at process start.
    if (!getenv("FBE_HELPER_LD")) {
      setenv("FBE_HELPER_LD", "1", 1);
      setenv("LD_LIBRARY_PATH", kLdPath, 1);
      execv("/proc/self/exe", argv);
      return Error("re-exec failed: %s", strerror(errno));
    }
    return UnlockCe(argc > 2 ? atoi(argv[2]) : 0);
  }
  fprintf(stderr,
          "usage: fbe_helper start-tee | mount-data [rw] | unlock-de | wipe-dalvik | "
          "umount-data | status | unlock-ce [user] (credential on stdin)\n");
  return 2;
}
