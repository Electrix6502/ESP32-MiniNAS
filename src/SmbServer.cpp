// SPDX-License-Identifier: LGPL-2.1-or-later
// File-system adapter for the vendored libsmb2 guest server.
#include "SmbServer.h"
#include "Storage.h"
#include "ProjectConfig.h"
#include "StatusLeds.h"
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <utime.h>
extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/smb2-errors.h>
#include <smb2/mininas-server.h>
}
#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace {
using Bytes = std::vector<uint8_t>;
struct SmbClientState {
    smb2_context *ctx = nullptr;
    bool trees[2] = {};
    bool ipc = false;
    uint64_t last = 0;
};
struct Handle {
    smb2_context *ctx = nullptr;
    uint64_t id = 0;
    unsigned volume = 0;
    int fd = -1;
    DIR *dir = nullptr;
    std::string path, pending, pattern;
    Bytes pipeReply;
    bool writable = false, deleting = false, pipe = false;
    uint32_t access = 0, sharing = 0;
};
std::array<SmbClientState, Config::SMB_MAX_CLIENTS> clientTable;
std::array<Handle, Config::SMB_MAX_HANDLES> fileTable;
uint64_t nextId = 1;
smb2_server server = {};
smb2_server_request_handlers handlers = {};
bool taskStarted = false;

void put(Bytes &b, size_t at, uint64_t v, size_t n) {
    if (b.size() < at + n)
        b.resize(at + n);
    for (size_t i = 0; i < n; ++i)
        b[at + i] = uint8_t(v >> (8 * i));
}
uint64_t get(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
uint64_t winTime(time_t t) {
    return (uint64_t(std::max<time_t>(t, 0)) + 11644473600ULL) * 10000000ULL;
}
Bytes utf16(const std::string &s) {
    auto *u = smb2_utf8_to_utf16(s.c_str());
    if (!u)
        return {};
    Bytes b(reinterpret_cast<uint8_t *>(u->val), reinterpret_cast<uint8_t *>(u->val) + 2 * u->len);
    free(u);
    return b;
}
std::string fromUtf16(const uint8_t *p, size_t size) {
    if (size % 2 || size > 960)
        return {};
    std::vector<uint16_t> aligned(size / 2);
    if (size)
        memcpy(aligned.data(), p, size);
    const char *s = smb2_utf16_to_utf8(aligned.data(), aligned.size());
    if (!s)
        return {};
    std::string out(s);
    free(const_cast<char *>(s));
    return out;
}
uint32_t attributes(const struct stat &s) {
    return S_ISDIR(s.st_mode) ? 0x10 : 0x20;
}
uint64_t allocation(const struct stat &s) {
    return (uint64_t(s.st_size) + 4095) & ~uint64_t(4095);
}
uint32_t statusForErrno(int e) {
    switch (e) {
    case ENOENT:
        return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR:
        return SMB2_STATUS_NOT_A_DIRECTORY;
    case EISDIR:
        return SMB2_STATUS_FILE_IS_A_DIRECTORY;
    case EEXIST:
        return SMB2_STATUS_OBJECT_NAME_COLLISION;
    case ENOSPC:
        return SMB2_STATUS_DISK_FULL;
    case ENOTEMPTY:
        return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
    case EMFILE:
    case ENFILE:
        return SMB2_STATUS_TOO_MANY_OPENED_FILES;
    case EACCES:
    case EPERM:
        return SMB2_STATUS_ACCESS_DENIED;
    case EINVAL:
        return SMB2_STATUS_INVALID_PARAMETER;
    default:
        return SMB2_STATUS_IO_DEVICE_ERROR;
    }
}
int error(smb2_context *c, uint16_t cmd, uint32_t status) {
    return mininas_error(c, cmd, status);
}
int ioError(smb2_context *c, uint16_t cmd) {
    const int cause = errno;
    Serial.printf("SMB: cmd %u I/O error %d (%s)\n", cmd, cause, strerror(cause));
    return error(c, cmd, statusForErrno(cause));
}
SmbClientState *client(smb2_context *c) {
    for (auto &x : clientTable)
        if (x.ctx == c)
            return &x;
    return nullptr;
}
int volume(smb2_context *c) {
    auto *x = client(c);
    uint32_t tid = mininas_tree_id(c);
    return x && tid >= 1 && tid <= 2 && x->trees[tid - 1] ? int(tid - 1) : -1;
}
bool ipc(smb2_context *c) {
    auto *x = client(c);
    return x && x->ipc && mininas_tree_id(c) == 3;
}
Handle *find(smb2_context *c, const uint8_t *id) {
    uint64_t number = get(id, 8);
    if (number == UINT64_MAX) {
        auto *x = client(c);
        number = x ? x->last : 0;
    }
    int v = volume(c);
    for (auto &h : fileTable)
        if (h.ctx == c && h.id == number &&
            (h.pipe ? ipc(c) : int(h.volume) == v))
            return &h;
    return nullptr;
}
bool sameOrChild(const std::string &path, const std::string &parent) {
    return path == parent ||
           (path.size() > parent.size() && path.compare(0, parent.size(), parent) == 0 &&
            path[parent.size()] == '/');
}
bool anotherOpen(const std::string &path, const Handle *except = nullptr) {
    for (const auto &h : fileTable)
        if (h.ctx && &h != except && sameOrChild(h.path, path))
            return true;
    return false;
}
bool writeAccess(uint32_t a) {
    return a & (0x40000000U | 0x10000000U | 2U | 4U | 0x10000U);
}
bool readAccess(uint32_t a) {
    return a & (0x80000000U | 0x10000000U | 1U);
}
uint32_t neededSharing(uint32_t a) {
    return (readAccess(a) ? 1 : 0) | ((a & (0x40000000U | 0x10000000U | 6U)) ? 2 : 0) |
           ((a & (0x10000000U | 0x10000U)) ? 4 : 0);
}
int closeHandle(Handle &h, bool removeFile) {
    int failure = 0;
    if (h.fd >= 0) {
        if (close(h.fd) != 0)
            failure = errno;
        h.fd = -1;
    }
    if (h.dir) {
        closedir(h.dir);
        h.dir = nullptr;
    }
    if (removeFile && h.deleting) {
        struct stat st;
        if (stat(h.path.c_str(), &st) == 0) {
            int r = S_ISDIR(st.st_mode) ? rmdir(h.path.c_str()) : unlink(h.path.c_str());
            if (r != 0)
                failure = errno;
        }
    }
    h = Handle{};
    return failure;
}
void fillCreate(smb2_create_reply *rep, const Handle &h, const struct stat &st) {
    rep->creation_time = winTime(st.st_ctime);
    rep->last_access_time = winTime(st.st_atime);
    rep->last_write_time = winTime(st.st_mtime);
    rep->change_time = winTime(st.st_mtime);
    rep->file_attributes = attributes(st);
    rep->allocation_size = S_ISDIR(st.st_mode) ? 0 : allocation(st);
    rep->end_of_file = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    rep->oplock_level = 0;
    for (unsigned i = 0; i < 8; ++i)
        rep->file_id[i] = uint8_t(h.id >> (8 * i));
    memcpy(rep->file_id + 8, "MiniNAS!", 8);
}
int destroy(smb2_server *, smb2_context *c) {
    StorageGuard guard;
    for (auto &h : fileTable)
        if (h.ctx == c)
            closeHandle(h, true);
    if (auto *x = client(c))
        *x = SmbClientState{};
    return 0;
}
int authorize(smb2_server *, smb2_context *c, const char *, const char *, const char *) {
    smb2_set_password(c, "");
    return 0;
}
int logoff(smb2_server *, smb2_context *c) {
    StorageGuard guard;
    for (auto &h : fileTable)
        if (h.ctx == c)
            closeHandle(h, true);
    if (auto *x = client(c)) {
        x->trees[0] = x->trees[1] = false;
        x->ipc = false;
        x->last = 0;
    }
    return 0;
}
int treeConnect(smb2_server *, smb2_context *c, smb2_tree_connect_request *req,
                smb2_tree_connect_reply *rep) {
    StorageGuard guard;
    std::string path = fromUtf16(reinterpret_cast<const uint8_t *>(req->path), req->path_length);
    auto pos = path.find_last_of(std::string("/") + char(92));
    std::string share = path.substr(pos == std::string::npos ? 0 : pos + 1);
    auto *x = client(c);
    if (x && strcasecmp(share.c_str(), "IPC$") == 0) {
        rep->share_type = SMB2_SHARE_TYPE_PIPE;
        rep->maximal_access = 0x001F01FF;
        x->ipc = true;
        return mininas_queue(c, smb2_cmd_tree_connect_reply_async(c, rep, 3, nullptr, nullptr));
    }
    for (unsigned v = 0; x && v < 2; ++v) {
        if (strcasecmp(share.c_str(), Storage::volumes[v].name.c_str()) != 0)
            continue;
        if (!Storage::volumes[v].ready)
            return error(c, SMB2_TREE_CONNECT, SMB2_STATUS_BAD_NETWORK_NAME);
        rep->share_type = SMB2_SHARE_TYPE_DISK;
        rep->share_flags = 0x30;
        rep->maximal_access = 0x001F01FF;
        x->trees[v] = true;
        return mininas_queue(c, smb2_cmd_tree_connect_reply_async(c, rep, v + 1, nullptr, nullptr));
    }
    return error(c, SMB2_TREE_CONNECT, SMB2_STATUS_BAD_NETWORK_NAME);
}
int treeDisconnect(smb2_server *, smb2_context *c, uint32_t tree) {
    StorageGuard guard;
    for (auto &h : fileTable)
        if (h.ctx == c && (h.volume + 1 == tree || (h.pipe && tree == 3)))
            closeHandle(h, true);
    if (auto *x = client(c))
        if (tree >= 1 && tree <= 2)
            x->trees[tree - 1] = false;
        else if (tree == 3)
            x->ipc = false;
    return 0;
}
int createFile(smb2_server *, smb2_context *c, smb2_create_request *req, smb2_create_reply *rep) {
    StorageGuard guard;
    int v = volume(c);
    std::string path;
    bool namedPipe = ipc(c) && req->name && strcasecmp(req->name, "srvsvc") == 0;
    if (v < 0 && !namedPipe)
        return error(c, SMB2_CREATE, SMB2_STATUS_NETWORK_NAME_DELETED);
    if (namedPipe) {
        Handle *h = nullptr;
        for (auto &slot : fileTable)
            if (!slot.ctx) { h = &slot; break; }
        if (!h)
            return error(c, SMB2_CREATE, SMB2_STATUS_TOO_MANY_OPENED_FILES);
        h->ctx = c;
        h->id = nextId++;
        h->path = "srvsvc";
        h->pipe = true;
        if (auto *x = client(c)) x->last = h->id;
        struct stat st = {};
        fillCreate(rep, *h, st);
        rep->file_attributes = 0;
        return 0;
    }
    if (!Storage::resolve(v, req->name ? req->name : "", path))
        return error(c, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_INVALID);
    if (req->desired_access & 0x12000000U)
        req->desired_access = 0x001F01FFU;
    if (req->desired_access & 0x80000000U)
        req->desired_access = (req->desired_access & ~0x80000000U) | 0x00120089U;
    if (req->desired_access & 0x40000000U)
        req->desired_access = (req->desired_access & ~0x40000000U) | 0x00120116U;
    for (auto &h : fileTable)
        if (h.ctx && h.path == path) {
            // The SMB share-access masks already express whether another
            // writer is allowed. Rejecting every second writable handle here
            // breaks the common Linux workflow of opening a file for data and
            // metadata updates at the same time.
            if (h.deleting || (neededSharing(req->desired_access) & ~h.sharing) ||
                (neededSharing(h.access) & ~req->share_access))
                return error(c, SMB2_CREATE, SMB2_STATUS_SHARING_VIOLATION);
        }
    Handle *h = nullptr;
    for (auto &slot : fileTable)
        if (!slot.ctx) {
            h = &slot;
            break;
        }
    if (!h)
        return error(c, SMB2_CREATE, SMB2_STATUS_TOO_MANY_OPENED_FILES);
    struct stat st;
    bool exists = stat(path.c_str(), &st) == 0;
    bool wantDir = req->create_options & SMB2_FILE_DIRECTORY_FILE,
         isDir = exists && S_ISDIR(st.st_mode);
    if (exists && !isDir && !S_ISREG(st.st_mode))
        return error(c, SMB2_CREATE, SMB2_STATUS_NOT_SUPPORTED);
    if (req->create_disposition > 5)
        return error(c, SMB2_CREATE, SMB2_STATUS_INVALID_PARAMETER);
    if (exists && req->create_disposition == SMB2_FILE_CREATE)
        return error(c, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_COLLISION);
    if (!exists && (req->create_disposition == SMB2_FILE_OPEN ||
                    req->create_disposition == SMB2_FILE_OVERWRITE))
        return error(c, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
    if (exists && wantDir && !isDir)
        return error(c, SMB2_CREATE, SMB2_STATUS_NOT_A_DIRECTORY);
    if (isDir && (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE))
        return error(c, SMB2_CREATE, SMB2_STATUS_FILE_IS_A_DIRECTORY);
    bool truncateFile = exists && !isDir &&
                        (req->create_disposition == 0 || req->create_disposition == 4 ||
                         req->create_disposition == 5);
    bool creates = !exists;
    // POSIX SMB clients commonly create a directory or a temporary save file
    // with metadata-only desired-access bits. The create disposition itself
    // is the mutating operation, so it must obtain a writable local handle.
    bool writable = writeAccess(req->desired_access) || creates || truncateFile;
    if ((truncateFile || (req->create_options & SMB2_FILE_DELETE_ON_CLOSE)) && anotherOpen(path))
        return error(c, SMB2_CREATE, SMB2_STATUS_SHARING_VIOLATION);
    if ((req->create_options & SMB2_FILE_DELETE_ON_CLOSE) &&
        (path == Storage::volumes[v].root || !(req->desired_access & (0x10000U | 0x10000000U))))
        return error(c, SMB2_CREATE, SMB2_STATUS_ACCESS_DENIED);
    if (!exists && wantDir) {
        if (mkdir(path.c_str(), 0777) != 0) {
            Serial.printf("SMB: mkdir %s failed (%d)\n", path.c_str(), errno);
            return ioError(c, SMB2_CREATE);
        }
        isDir = true;
    }
    int fd = -1;
    DIR *dir = nullptr;
    if (isDir) {
        dir = opendir(path.c_str());
        if (!dir)
            return ioError(c, SMB2_CREATE);
    } else {
        int flags = writable ? O_RDWR : O_RDONLY;
        if (!exists)
            flags |= O_CREAT;
        if (truncateFile)
            flags |= O_TRUNC;
        fd = open(path.c_str(), flags, 0666);
        // The ESP-IDF FatFs VFS used by SD accepts O_WRONLY|O_CREAT on all
        // cards, while some versions reject the O_RDWR creation combination.
        // A just-created SMB file is written before it is read again, so this
        // fallback preserves the requested operation and is reopened normally
        // on its next CREATE request.
        if (fd < 0 && creates && writable && errno == ENOENT)
            fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
            return ioError(c, SMB2_CREATE);
    }
    if (stat(path.c_str(), &st) != 0) {
        int saved = errno;
        if (fd >= 0)
            close(fd);
        if (dir)
            closedir(dir);
        return error(c, SMB2_CREATE, statusForErrno(saved));
    }
    h->ctx = c;
    h->volume = v;
    h->id = nextId++;
    h->path = path;
    h->fd = fd;
    h->dir = dir;
    h->writable = writable;
    h->access = req->desired_access;
    h->sharing = req->share_access;
    h->deleting = req->create_options & SMB2_FILE_DELETE_ON_CLOSE;
    if (auto *x = client(c))
        x->last = h->id;
    rep->create_action = creates ? 2 : (truncateFile ? 3 : 1);
    fillCreate(rep, *h, st);
    return 0;
}
int closeFile(smb2_server *, smb2_context *c, smb2_close_request *req, smb2_close_reply *rep) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_CLOSE, SMB2_STATUS_FILE_CLOSED);
    memset(rep, 0, sizeof(*rep));
    int e = closeHandle(*h, true);
    if (e)
        return error(c, SMB2_CLOSE, statusForErrno(e));
    return 0;
}
int flushFile(smb2_server *, smb2_context *c, smb2_flush_request *req) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_FLUSH, SMB2_STATUS_FILE_CLOSED);
    if (h->fd >= 0 && fsync(h->fd) != 0)
        return ioError(c, SMB2_FLUSH);
    return 0;
}
int readFile(smb2_server *, smb2_context *c, smb2_read_request *req, smb2_read_reply *rep) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_READ, SMB2_STATUS_FILE_CLOSED);
    if (h->fd < 0)
        return error(c, SMB2_READ, SMB2_STATUS_FILE_IS_A_DIRECTORY);
    if (!readAccess(h->access))
        return error(c, SMB2_READ, SMB2_STATUS_ACCESS_DENIED);
    if (req->length > Config::SMB_IO_SIZE || req->offset > INT32_MAX)
        return error(c, SMB2_READ, SMB2_STATUS_INVALID_PARAMETER);
    rep->data = static_cast<uint8_t *>(malloc(std::max<uint32_t>(1, req->length)));
    if (!rep->data)
        return error(c, SMB2_READ, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    if (lseek(h->fd, req->offset, SEEK_SET) < 0) {
        int e = errno;
        free(rep->data);
        rep->data = nullptr;
        return error(c, SMB2_READ, statusForErrno(e));
    }
    ssize_t n = read(h->fd, rep->data, req->length);
    if (n <= 0 && req->length) {
        int e = errno;
        free(rep->data);
        rep->data = nullptr;
        return error(c, SMB2_READ, n == 0 ? SMB2_STATUS_END_OF_FILE : statusForErrno(e));
    }
    if (!req->length) {
        free(rep->data);
        rep->data = nullptr;
    }
    rep->data_length = n;
    rep->data_remaining = 0;
    if (n > 0)
        StatusLeds::readActivity();
    return 0;
}
int writeFile(smb2_server *, smb2_context *c, smb2_write_request *req, smb2_write_reply *rep) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_WRITE, SMB2_STATUS_FILE_CLOSED);
    if (h->fd < 0 || !h->writable)
        return error(c, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
    if (req->length > Config::SMB_IO_SIZE || req->offset > INT32_MAX ||
        req->length > uint64_t(INT32_MAX) - req->offset)
        return error(c, SMB2_WRITE, 0xC0000904U);
    if (lseek(h->fd, req->offset, SEEK_SET) < 0)
        return ioError(c, SMB2_WRITE);
    size_t done = 0;
    while (done < req->length) {
        ssize_t n = write(h->fd, req->buf + done, req->length - done);
        if (n <= 0)
            return error(c, SMB2_WRITE, statusForErrno(n < 0 ? errno : ENOSPC));
        done += n;
    }
    if ((req->flags & 1) && fsync(h->fd) != 0)
        return ioError(c, SMB2_WRITE);
    rep->count = done;
    rep->remaining = 0;
    if (done)
        StatusLeds::writeActivity();
    return 0;
}
void basicInfo(Bytes &b, size_t o, const struct stat &st) {
    put(b, o, winTime(st.st_ctime), 8);
    put(b, o + 8, winTime(st.st_atime), 8);
    put(b, o + 16, winTime(st.st_mtime), 8);
    put(b, o + 24, winTime(st.st_mtime), 8);
    put(b, o + 32, attributes(st), 4);
    put(b, o + 36, 0, 4);
}
void standardInfo(Bytes &b, size_t o, const Handle &h, const struct stat &st) {
    put(b, o, h.dir ? 0 : allocation(st), 8);
    put(b, o + 8, h.dir ? 0 : st.st_size, 8);
    put(b, o + 16, 1, 4);
    put(b, o + 20, h.deleting, 1);
    put(b, o + 21, h.dir != nullptr, 1);
    put(b, o + 22, 0, 2);
}
int queryInfo(smb2_server *, smb2_context *c, smb2_query_info_request *req,
              smb2_query_info_reply *) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_QUERY_INFO, SMB2_STATUS_FILE_CLOSED);
    struct stat st;
    if (stat(h->path.c_str(), &st) != 0)
        return ioError(c, SMB2_QUERY_INFO);
    Bytes b;
    if (req->info_type == SMB2_0_INFO_FILE) {
        switch (req->file_info_class) {
        case 4:
            basicInfo(b, 0, st);
            break;
        case 5:
            standardInfo(b, 0, *h, st);
            break;
        case 6:
            put(b, 0, st.st_ino, 8);
            break;
        case 7:
            put(b, 0, 0, 4);
            break;
        case 8:
            put(b, 0, h->access, 4);
            break;
        case 9:
        case 21: {
            auto name =
                utf16(std::string(1, char(92)) +
                      h->path.substr(Storage::volumes[h->volume].root.size() +
                                     (h->path == Storage::volumes[h->volume].root ? 0 : 1)));
            put(b, 0, name.size(), 4);
            b.insert(b.end(), name.begin(), name.end());
            break;
        }
        case 14:
            put(b, 0, h->fd >= 0 ? lseek(h->fd, 0, SEEK_CUR) : 0, 8);
            break;
        case 16:
        case 17:
            put(b, 0, 0, 4);
            break;
        case 18: {
            basicInfo(b, 0, st);
            standardInfo(b, 40, *h, st);
            put(b, 64, st.st_ino, 8);
            put(b, 72, 0, 4);
            put(b, 76, h->access, 4);
            put(b, 80, h->fd >= 0 ? lseek(h->fd, 0, SEEK_CUR) : 0, 8);
            put(b, 88, 0, 4);
            put(b, 92, 0, 4);
            auto name = utf16(h->path.substr(Storage::volumes[h->volume].root.size()));
            put(b, 96, name.size(), 4);
            b.insert(b.end(), name.begin(), name.end());
            break;
        }
        case 22: {
            auto n = utf16("::$DATA");
            b.resize(24);
            put(b, 4, n.size(), 4);
            put(b, 8, st.st_size, 8);
            put(b, 16, allocation(st), 8);
            b.insert(b.end(), n.begin(), n.end());
            break;
        }
        case 34:
            basicInfo(b, 0, st);
            put(b, 32, h->dir ? 0 : allocation(st), 8);
            put(b, 40, h->dir ? 0 : st.st_size, 8);
            put(b, 48, attributes(st), 4);
            put(b, 52, 0, 4);
            break;
        case 35:
            put(b, 0, attributes(st), 4);
            put(b, 4, 0, 4);
            break;
        default:
            return error(c, SMB2_QUERY_INFO, SMB2_STATUS_INVALID_INFO_CLASS);
        }
    } else if (req->info_type == SMB2_0_INFO_FILESYSTEM) {
        uint64_t capacity = Storage::total(h->volume),
                 used = std::min(Storage::used(h->volume), capacity), total = capacity / 4096,
                 free = (capacity - used) / 4096;
        switch (req->file_info_class) {
        case 1: {
            auto name = utf16(Storage::volumes[h->volume].name);
            b.resize(18);
            put(b, 8, h->volume + 1, 4);
            put(b, 12, name.size(), 4);
            b.insert(b.end(), name.begin(), name.end());
            break;
        }
        case 3:
            put(b, 0, total, 8);
            put(b, 8, free, 8);
            put(b, 16, 8, 4);
            put(b, 20, 512, 4);
            break;
        case 4:
            put(b, 0, 7, 4);
            put(b, 4, 0, 4);
            break;
        case 5: {
            auto name = utf16(h->volume == 0 ? "LittleFS" : "FAT32");
            put(b, 0, h->volume == 0 ? 0x7 : 0x6, 4);
            put(b, 4, 255, 4);
            put(b, 8, name.size(), 4);
            b.insert(b.end(), name.begin(), name.end());
            break;
        }
        case 7:
            put(b, 0, total, 8);
            put(b, 8, free, 8);
            put(b, 16, free, 8);
            put(b, 24, 8, 4);
            put(b, 28, 512, 4);
            break;
        case 11:
            b.resize(28);
            for (unsigned i = 0; i < 4; ++i)
                put(b, 4 * i, 512, 4);
            break;
        default:
            return error(c, SMB2_QUERY_INFO, SMB2_STATUS_INVALID_INFO_CLASS);
        }
    } else
        return error(c, SMB2_QUERY_INFO, SMB2_STATUS_NOT_SUPPORTED);
    if (b.size() > req->output_buffer_length)
        return error(c, SMB2_QUERY_INFO, SMB2_STATUS_BUFFER_TOO_SMALL);
    smb2_query_info_reply rep = {};
    rep.output_buffer = b.data();
    rep.output_buffer_length = b.size();
    return mininas_queue(c, smb2_cmd_query_info_reply_async(c, req, &rep, nullptr, nullptr));
}
int queryDir(smb2_server *, smb2_context *c, smb2_query_directory_request *req,
             smb2_query_directory_reply *) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_QUERY_DIRECTORY, SMB2_STATUS_FILE_CLOSED);
    if (!h->dir)
        return error(c, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NOT_A_DIRECTORY);
    unsigned base = 0;
    switch (req->file_information_class) {
    case 1:
        base = 64;
        break;
    case 2:
        base = 68;
        break;
    case 3:
        base = 94;
        break;
    case 12:
        base = 12;
        break;
    case 37:
        base = 104;
        break;
    case 38:
        base = 80;
        break;
    default:
        return error(c, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INVALID_INFO_CLASS);
    }
    if (req->flags & 4)
        return error(c, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NOT_SUPPORTED);
    if (req->flags & (1 | 16)) {
        closedir(h->dir);
        h->dir = opendir(h->path.c_str());
        h->pending.clear();
        if (!h->dir)
            return ioError(c, SMB2_QUERY_DIRECTORY);
    }
    if (req->name && *req->name)
        h->pattern = req->name;
    if (h->pattern.empty() || h->pattern == "*.*")
        h->pattern = "*";
    size_t room = std::min<uint32_t>(req->output_buffer_length, Config::SMB_IO_SIZE);
    Bytes b;
    size_t previous = SIZE_MAX;
    while (true) {
        std::string name;
        if (!h->pending.empty()) {
            name = h->pending;
            h->pending.clear();
        } else {
            auto *de = readdir(h->dir);
            if (!de)
                break;
            name = de->d_name;
        }
        if (name == "." || name == ".." || fnmatch(h->pattern.c_str(), name.c_str(), 0) != 0)
            continue;
        struct stat st;
        if (stat((h->path + "/" + name).c_str(), &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
            continue;
        auto n = utf16(name);
        if (n.empty())
            continue;
        size_t size = (base + n.size() + 7) & ~size_t(7);
        if (b.size() + size > room) {
            h->pending = name;
            if (b.empty())
                return error(c, SMB2_QUERY_DIRECTORY, SMB2_STATUS_BUFFER_TOO_SMALL);
            break;
        }
        size_t o = b.size();
        b.resize(o + size);
        if (previous != SIZE_MAX)
            put(b, previous, o - previous, 4);
        previous = o;
        if (req->file_information_class == 12)
            put(b, o + 8, n.size(), 4);
        else {
            put(b, o + 8, winTime(st.st_ctime), 8);
            put(b, o + 16, winTime(st.st_atime), 8);
            put(b, o + 24, winTime(st.st_mtime), 8);
            put(b, o + 32, winTime(st.st_mtime), 8);
            put(b, o + 40, S_ISDIR(st.st_mode) ? 0 : st.st_size, 8);
            put(b, o + 48, S_ISDIR(st.st_mode) ? 0 : allocation(st), 8);
            put(b, o + 56, attributes(st), 4);
            put(b, o + 60, n.size(), 4);
            if (req->file_information_class == 37)
                put(b, o + 96, st.st_ino, 8);
            if (req->file_information_class == 38)
                put(b, o + 72, st.st_ino, 8);
        }
        std::copy(n.begin(), n.end(), b.begin() + o + base);
        if (req->flags & 2)
            break;
    }
    if (b.empty())
        return error(c, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NO_MORE_FILES);
    smb2_query_directory_reply rep = {};
    rep.output_buffer = b.data();
    rep.output_buffer_length = b.size();
    return mininas_queue(c, smb2_cmd_query_directory_reply_async(c, req, &rep, nullptr, nullptr));
}
int setInfo(smb2_server *, smb2_context *c, smb2_set_info_request *req) {
    StorageGuard guard;
    auto *h = find(c, req->file_id);
    if (!h)
        return error(c, SMB2_SET_INFO, SMB2_STATUS_FILE_CLOSED);
    if (req->info_type != SMB2_0_INFO_FILE)
        return error(c, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
    auto *p = static_cast<uint8_t *>(req->input_data);
    size_t n = req->buffer_length;
    if (!p || !n)
        return error(c, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
    switch (req->file_info_class) {
    case 4: {
        if (n < 40)
            return error(c, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
        uint64_t stamp = get(p + 16, 8);
        if (stamp && stamp != UINT64_MAX) {
            uint64_t seconds = stamp / 10000000ULL;
            if (seconds < 11644473600ULL || seconds - 11644473600ULL > INT32_MAX)
                return error(c, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
            utimbuf times;
            times.actime = times.modtime = seconds - 11644473600ULL;
            if (utime(h->path.c_str(), &times) != 0)
                return ioError(c, SMB2_SET_INFO);
        }
        break;
    }
    case 10: {
        if (n < 20 || get(p + 8, 8) != 0 || get(p + 16, 4) > n - 20 || get(p + 16, 4) % 2)
            return error(c, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
        if (!(h->access & (0x10000U | 0x10000000U)))
            return error(c, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
        if (h->path == Storage::volumes[h->volume].root || anotherOpen(h->path, h))
            return error(c, SMB2_SET_INFO, SMB2_STATUS_SHARING_VIOLATION);
        std::string name = fromUtf16(p + 20, get(p + 16, 4)), target;
        if (name.empty() || !Storage::resolve(h->volume, name.c_str(), target) ||
            target == Storage::volumes[h->volume].root)
            return error(c, SMB2_SET_INFO, SMB2_STATUS_OBJECT_NAME_INVALID);
        if (anotherOpen(target, h))
            return error(c, SMB2_SET_INFO, SMB2_STATUS_SHARING_VIOLATION);
        struct stat existing;
        if (!p[0] && target != h->path && stat(target.c_str(), &existing) == 0)
            return error(c, SMB2_SET_INFO, SMB2_STATUS_OBJECT_NAME_COLLISION);
        // esp_littlefs does not permit renaming an open object. SMB however
        // expects FileRenameInformation to work on the handle used for the
        // request, so temporarily release this handle and restore it below.
        const bool directory = h->dir != nullptr;
        const int flags = h->writable ? O_RDWR : O_RDONLY;
        const std::string source = h->path;
        if (h->fd >= 0) {
            close(h->fd);
            h->fd = -1;
        }
        if (h->dir) {
            closedir(h->dir);
            h->dir = nullptr;
        }
        if (rename(source.c_str(), target.c_str()) != 0) {
            if (directory)
                h->dir = opendir(source.c_str());
            else
                h->fd = open(source.c_str(), flags);
            return ioError(c, SMB2_SET_INFO);
        }
        h->path = target;
        if (directory)
            h->dir = opendir(h->path.c_str());
        else
            h->fd = open(h->path.c_str(), flags);
        if ((directory && !h->dir) || (!directory && h->fd < 0))
            return ioError(c, SMB2_SET_INFO);
        break;
    }
    case 13:
        if (!(h->access & (0x10000U | 0x10000000U)) || h->path == Storage::volumes[h->volume].root)
            return error(c, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
        if (p[0] && anotherOpen(h->path, h))
            return error(c, SMB2_SET_INFO, SMB2_STATUS_SHARING_VIOLATION);
        if (p[0] && h->dir) {
            DIR *d = opendir(h->path.c_str());
            if (!d)
                return ioError(c, SMB2_SET_INFO);
            bool empty = true;
            while (auto *e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                    empty = false;
                    break;
                }
            closedir(d);
            if (!empty)
                return error(c, SMB2_SET_INFO, SMB2_STATUS_DIRECTORY_NOT_EMPTY);
        }
        h->deleting = p[0] != 0;
        break;
    case 19:
    case 20: {
        if (n < 8 || !h->writable || h->fd < 0)
            return error(c, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
        uint64_t size = get(p, 8);
        if (size > INT32_MAX)
            return error(c, SMB2_SET_INFO, 0xC0000904U);
        if (req->file_info_class == 20 && ftruncate(h->fd, size) != 0)
            return ioError(c, SMB2_SET_INFO);
        break;
    }
    default:
        return error(c, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
    }
    return 0;
}
void u16(Bytes &b, uint16_t v) { put(b, b.size(), v, 2); }
void u32(Bytes &b, uint32_t v) { put(b, b.size(), v, 4); }
void ndrString(Bytes &b, const char *s) {
    size_t n = strlen(s) + 1;
    u32(b, n); u32(b, 0); u32(b, n);
    for (size_t i = 0; i < n; ++i) u16(b, uint8_t(s[i]));
    while (b.size() & 3) b.push_back(0);
}
void dceHeader(Bytes &b, uint8_t type, uint32_t call, size_t header) {
    b.resize(header);
    b[0] = 5; b[2] = type; b[3] = 3; b[4] = 0x10;
    put(b, 12, call, 4);
}
int ioctl(smb2_server *, smb2_context *c, smb2_ioctl_request *req, smb2_ioctl_reply *rep) {
    auto *h = find(c, req->file_id);
    if (!h || !h->pipe || req->ctl_code != SMB2_FSCTL_PIPE_TRANSCEIVE || !req->input ||
        req->input_count < 16)
        return error(c, SMB2_IOCTL, SMB2_STATUS_NOT_SUPPORTED);
    auto *in = static_cast<const uint8_t *>(req->input);
    Bytes out;
    uint32_t call = get(in + 12, 4);
    if (in[2] == 11) { // DCERPC bind -> bind_ack, NDR32 accepted
        dceHeader(out, 12, call, 56);
        put(out, 16, 4280, 2); put(out, 18, 4280, 2);
        put(out, 24, 0, 2); // empty secondary address
        out[28] = 1; // one presentation-context result
        put(out, 32, 0, 2); // acceptance
        const uint8_t ndr[] = {0x04,0x5d,0x88,0x8a,0xeb,0x1c,0xc9,0x11,0x9f,0xe8,0x08,0x00,0x2b,0x10,0x48,0x60,0x02,0x00,0x00,0x00};
        memcpy(out.data() + 36, ndr, sizeof(ndr));
    } else if (in[2] == 0) { // NetrShareEnum response (ShareInfo level 1)
        unsigned count = Storage::volumes[0].ready + Storage::volumes[1].ready;
        Bytes stub;
        u32(stub, 1); u32(stub, 1); u32(stub, 0x20004);
        u32(stub, count); u32(stub, count ? 0x20008 : 0);
        if (count) {
            u32(stub, count);
            for (unsigned v = 0; v < 2; ++v) if (Storage::volumes[v].ready) {
                u32(stub, 0x20010 + v * 8); u32(stub, 0); u32(stub, 0x20011 + v * 8);
            }
            for (unsigned v = 0; v < 2; ++v) if (Storage::volumes[v].ready) {
                ndrString(stub, Storage::volumes[v].name.c_str()); ndrString(stub, "ESP32 MiniNAS");
            }
        }
        u32(stub, count); u32(stub, 0); u32(stub, 0);
        dceHeader(out, 2, call, 24);
        put(out, 16, stub.size(), 4);
        out.insert(out.end(), stub.begin(), stub.end());
    } else return error(c, SMB2_IOCTL, SMB2_STATUS_NOT_SUPPORTED);
    put(out, 8, out.size(), 2);
    h->pipeReply = std::move(out);
    rep->output = h->pipeReply.data(); rep->output_count = h->pipeReply.size();
    rep->flags = SMB2_0_IOCTL_IS_FSCTL;
    return 0;
}
int lockFile(smb2_server *, smb2_context *c, smb2_lock_request *) {
    // LittleFS/FATFS have no cross-client byte-range lock primitive. Accept
    // advisory locks so POSIX SMB clients can edit files; actual conflicting
    // opens are still guarded by the SMB share-access checks above.
    return 0;
}
int notify(smb2_server *, smb2_context *c, smb2_change_notify_request *,
           smb2_change_notify_reply *) {
    return error(c, SMB2_CHANGE_NOTIFY, SMB2_STATUS_NOT_SUPPORTED);
}
int echo(smb2_server *, smb2_context *) {
    return 0;
}
void onError(smb2_context *, const char *message) {
    if (message && *message)
        fprintf(stderr, "SMB: %s\n", message);
}
void newClient(smb2_context *c, void *) {
    StorageGuard guard;
    SmbClientState *available = nullptr;
    for (auto &slot : clientTable)
        if (!slot.ctx) {
            available = &slot;
            break;
        }
    if (!available) {
        mininas_drop(c);
        return;
    }
    available->ctx = c;
    smb2_register_error_callback(c, onError);
    smb2_set_version(c, SMB2_VERSION_ANY);
    smb2_set_passthrough(c, 1);
    smb2_set_password(c, "");
}
#ifdef ARDUINO
void serverTask(void *) {
    while (true) {
        int r = SmbServer::serve(Config::SMB_PORT);
        Serial.printf("SMB server stopped (%d); retry in 3 s\n", r);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif
} // namespace
namespace SmbServer {
int serve(uint16_t port) {
    handlers.destruction_event = destroy;
    handlers.authorize_user = authorize;
    handlers.logoff_cmd = logoff;
    handlers.session_established = echo;
    handlers.tree_connect_cmd = treeConnect;
    handlers.tree_disconnect_cmd = treeDisconnect;
    handlers.create_cmd = createFile;
    handlers.close_cmd = closeFile;
    handlers.flush_cmd = flushFile;
    handlers.read_cmd = readFile;
    handlers.write_cmd = writeFile;
    handlers.query_directory_cmd = queryDir;
    handlers.query_info_cmd = queryInfo;
    handlers.set_info_cmd = setInfo;
    handlers.ioctl_cmd = ioctl;
    handlers.lock_cmd = lockFile;
    handlers.change_notify_cmd = notify;
    handlers.echo_cmd = echo;
    server = {};
    server.handlers = &handlers;
    server.port = port;
    server.allow_anonymous = 1;
    server.signing_enabled = 0;
    server.max_transact_size = server.max_read_size = server.max_write_size = Config::SMB_IO_SIZE;
    snprintf(server.hostname, sizeof(server.hostname), "%s", Config::HOSTNAME);
    memcpy(server.guid, "ESP32-MiniNAS-v1", 16);
#ifdef ARDUINO
    uint64_t mac = ESP.getEfuseMac();
    memcpy(server.guid + 8, &mac, sizeof(mac));
#endif
    int result = smb2_serve_port(&server, Config::SMB_MAX_CLIENTS, newClient, nullptr);
    {
        StorageGuard guard;
        for (auto &h : fileTable)
            if (h.ctx)
                closeHandle(h, true);
        for (auto &x : clientTable)
            x = SmbClientState{};
    }
    return result;
}
void begin() {
#ifdef ARDUINO
    if (taskStarted)
        return;
    taskStarted =
        xTaskCreate(serverTask, "smb", Config::SMB_TASK_STACK, nullptr, 1, nullptr) == pdPASS;
    if (!taskStarted)
        Serial.println("SMB: Task konnte nicht angelegt werden");
#endif
}
bool ready() {
    return server.listener_ready != 0;
}
void stop() {
    server.stop_requested = 1;
}
unsigned clients() {
    StorageGuard g;
    unsigned n = 0;
    for (auto &x : clientTable)
        n += x.ctx != nullptr;
    return n;
}
unsigned handles() {
    StorageGuard g;
    unsigned n = 0;
    for (auto &x : fileTable)
        n += x.ctx != nullptr;
    return n;
}
bool writeOpen(const std::string &absolute) {
    StorageGuard g;
    for (auto &h : fileTable)
        if (h.ctx && h.writable && sameOrChild(absolute, h.path))
            return true;
    return false;
}
} // namespace SmbServer
