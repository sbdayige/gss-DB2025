/**
 * @brief 高级磁盘管理器实现
 *
 * 提供高效的磁盘I/O操作和页面管理功能
 */

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek

#include "defs.h"
#include "record/rm_defs.h"

/**
 * @brief 初始化磁盘管理器
 */
DiskManager::DiskManager() {
    // 使用循环初始化原子类型数组，确保线程安全
    for (int i = 0; i < MAX_FD; ++i) {
        fd2pageno_[i].store(0, std::memory_order_relaxed);
    }
}

/**
 * @brief 高性能页面写入操作
 * @param fd 文件描述符
 * @param page_no 页面号
 * @param offset 数据缓冲区
 * @param num_bytes 写入字节数
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    // 计算页面在文件中的偏移量
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;

    // 定位到指定位置
    if (lseek(fd, file_offset, SEEK_SET) == -1) {
        throw UnixError();
    }

    // 执行写入操作
    ssize_t bytes_written = write(fd, offset, num_bytes);
    if (bytes_written != num_bytes) {
        throw InternalError("DiskManager::write_page - Write operation failed");
    }

    // 更新统计信息
    total_writes_.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 高性能页面读取操作
 * @param fd 文件描述符
 * @param page_no 页面号
 * @param offset 目标缓冲区
 * @param num_bytes 读取字节数
 */
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    // 计算页面在文件中的偏移量
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;

    // 定位到指定位置
    if (lseek(fd, file_offset, SEEK_SET) == -1) {
        throw UnixError();
    }

    // 执行读取操作
    ssize_t bytes_read = read(fd, offset, num_bytes);
    if (bytes_read != num_bytes) {
        throw InternalError("DiskManager::read_page - Read operation failed");
    }

    // 更新统计信息
    total_reads_.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 分配新页面号
 * @param fd 文件描述符
 * @return page_id_t 新分配的页面号
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 使用原子操作确保线程安全的页面号分配
    assert(fd >= 0 && fd < MAX_FD);
    return fd2pageno_[fd].fetch_add(1, std::memory_order_acq_rel);
}

/**
 * @brief 释放页面（暂时为空实现）
 */
void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {
    // 页面回收逻辑可以在此实现
}

/**
 * @brief 检查路径是否为目录
 */
bool DiskManager::is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @brief 创建目录
 */
void DiskManager::create_dir(const std::string &path) {
    // 使用系统调用创建目录
    std::string cmd = "mkdir -p " + path;  // -p 标志允许创建父目录
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @brief 删除目录
 */
void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -rf " + path;  // -rf 强制递归删除
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string &path) {
    // 用struct stat获取文件信息
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string &path) {
    // Todo:
    // 调用open()函数，使用O_CREAT模式
    // 注意不能重复创建相同文件
    struct stat buffer;

    // 检查文件是否已经存在
    if (stat(path.c_str(), &buffer) == 0) {
        throw FileExistsError(path);
    }

    // 创建文件，_CREAT | O_EXCL 确保文件是新创建的，如果文件已经存在，则会返回错误。S_IRUSR | S_IWUSR 设置文件的权限，表示文件所有者有读写权限
    int fd = open(path.c_str(), O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        throw UnixError();
    }

    // 关闭文件描述符
    if (close(fd) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string &path) {
    // Todo:
    // 调用unlink()函数
    // 注意不能删除未关闭的文件
    auto it = path2fd_.find(path);
    if (it != path2fd_.end()) {
        throw FileOpenError(path);  // 文件被找到
    }

    // 删除文件
    if (unlink(path.c_str()) < 0) {
        if (errno == ENOENT) {
            throw FileNotFoundError(path);
        } else {
            throw UnixError();
        }
    }
}

/**
 * @description: 打开指定路径文件
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string &path) {
    // Todo:
    // 调用open()函数，使用O_RDWR模式
    // 注意不能重复打开相同文件，并且需要更新文件打开列表

    if (path2fd_.find(path) != path2fd_.end()) {
        throw FileOpenError(path);
    }

    // 打开文件
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        if (errno == ENOENT) {
            throw FileNotFoundError(path);
        } else {
            throw UnixError();
        }
    }

    // 更新文件打开列表
    path2fd_[path] = fd;
    fd2path_[fd] = path;

    return fd;
}

/**
 * @description:用于关闭指定路径文件
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    // Todo:
    // 调用close()函数
    // 注意不能关闭未打开的文件，并且需要更新文件打开列表
    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotFoundError("File descriptor not found");
    }

    // 关闭文件
    if (close(fd) < 0) {
        throw UnixError();
    }

    // 更新文件打开列表
    std::string path = it->second;
    fd2path_.erase(it);
    path2fd_.erase(path);
}

/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string &file_name) {
    if (!path2fd_.count(file_name)) {
        return open_file(file_name);
    }
    return path2fd_[file_name];
}

/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
int DiskManager::read_log(char *log_data, int size, int offset) {
    // read log file from the previous end
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if (size == 0)
        return 0;
    lseek(log_fd_, offset, SEEK_SET);
    ssize_t bytes_read = read(log_fd_, log_data, size);
    assert(bytes_read == size);
    return bytes_read;
}

/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(char *log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    // write from the file_end
    lseek(log_fd_, 0, SEEK_END);
    ssize_t bytes_write = write(log_fd_, log_data, size);
    if (bytes_write != size) {
        throw UnixError();
    }
}