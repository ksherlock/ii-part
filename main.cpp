// clang++ -std=c++14 -Wall main.cpp `pkg-config fuse --cflags --libs` -o ii-part-fuse

/*
 * Thanks to: 
 * - R. Belmont - MAME/a2zipdrive.cpp, a2vulcan.c
 * - Andy McFadden - https://github.com/fadden/ciderpress
 * - Bobbi Manners - https://github.com/bobbimanners/mdttool
 * - Jon Lasser - https://github.com/disappearinjon/microdrive
 */


#include <unistd.h>
#include <fcntl.h>
#include <err.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>


#define FUSE_USE_VERSION 31
#include <fuse.h>



#ifdef __APPLE__
#include <sys/disk.h>
#endif

#ifdef __linux__
#include <sys/mount.h> 
#endif

#ifdef __sun__
#include <sys/dkio.h>
#endif


#ifdef __FREEBSD__
#include <sys/disk.h>
#endif

#ifdef __minix
#include <minix/partition.h>
#endif

void help(int);

struct options
{
	const char *filename;
	const char *mountpoint;
	bool verbose;
	bool rw;
} options;

enum {
	OPTION_HELP = 0,
	OPTION_RW,
};

#define OPTION(t, p)                           \
    { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
	FUSE_OPT_KEY("-h", OPTION_HELP),
	FUSE_OPT_KEY("--help", OPTION_HELP),
	FUSE_OPT_KEY("rw", OPTION_RW),

	OPTION("-v", verbose),
	OPTION("--verbose", verbose),
	FUSE_OPT_END
};


static int part_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs) {

	switch (key) {
		case OPTION_HELP:
			help(0);
			break;

		case OPTION_RW:
			options.rw = true;
			return 1; // save for fuse.

		case FUSE_OPT_KEY_NONOPT:
			if (!options.filename) {
				options.filename = arg;
				return 0;
			}
			if (!options.mountpoint) {
				options.mountpoint = arg;
				return 1; // save for fuse.
			}
			break;
	}
	return 1;
}


struct file_info
{
	std::string name;
	off_t start;
	off_t size;

	bool operator==(const char *s)
	{
		return !strcmp(name.c_str(), s);
	}
	bool operator==(const std::string &s)
	{
		return name == s;
	}
};


// as there will be 16 or fewer partitions, a vector is fine.
std::vector<file_info> files;
int fd;


inline uint16_t read16(const unsigned char *data)
{
	return data[0] + (data[1] << 8);
}

inline uint32_t read24(const unsigned char *data)
{
	return data[0] + (data[1] << 8) + (data[2] << 16);
}

inline uint32_t read32(const unsigned char *data)
{
	return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}




static bool is_microdrive(const unsigned char *data)
{
	return data[0] == 0xca && data[1] == 0xcc && read32(data + 0x20) == 256;
}

static void parse_microdrive(const unsigned char *data)
{
	if (options.verbose) {
		fputs("Found MicroDrive partition\n", stdout);
	}

	int pcount;

	pcount = data[0x0c];
	for (int i = 0; i < pcount; ++i) {

		struct file_info f;
		std::string name;

		name = std::string("MicroDrive1-") + std::to_string(i + 1);
		unsigned start = read32(data + 0x20 + i * 4);
		unsigned count = read24(data + 0x40 + i * 4);

		if (options.verbose) {
			printf("%d: %-20s %8u %8u\n", i + 1, name.c_str(), start, count);
		}

		f.start = start * 512;
		f.size = count * 512;
		f.name = std::move(name);
		files.emplace_back(std::move(f));
	}


	pcount = data[0x0d];
	for (int i = 0; i < pcount; ++i) {

		struct file_info f;
		std::string name;

		name = std::string("MicroDrive2-") + std::to_string(i + 1);
		unsigned start = read32(data + 0x80 + i * 4);
		unsigned count = read24(data + 0xa0 + i * 4);

		if (options.verbose) {
			printf("%d: %-20s %8u %8u\n", i + 1, name.c_str(), start, count);
		}

		f.start = start * 512;
		f.size = count * 512;
		f.name = std::move(name);
		files.emplace_back(std::move(f));
	}
}


static bool is_focus(const unsigned char *data)
{
	return !memcmp(data, "Parsons Engin.", 15);
}

static bool is_zip(const unsigned char *data)
{
	return !memcmp(data, "Zip Technolog.", 15);
}

off_t file_size(int fd)
{
	struct stat st;
	int ok;

	ok = fstat(fd, &st);
	if (ok < 0) return -1;

	if (S_ISREG(st.st_mode)) return st.st_size;

	if (S_ISBLK(st.st_mode)) {

		#if defined(__APPLE__)
		uint32_t blockSize = 0; // 32 bit
		uint64_t blockCount = 0; // 64 bit

		if (::ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize) < 0)
			err(1, "Unable to determine block size");


		if (::ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount) < 0)
			err(1, "Unable to determine block count");

		return blockSize * blockCount;
		#endif

		#if defined(__linux__)

		#if defined(BLKGETSIZE64)
		unsigned long long bytes = 0;
		if (::ioctl(fd, BLKGETSIZE64, &bytes) == 0)
			return bytes;
		#endif


		unsigned long blocks = 0;
		if (::ioctl(fd, BLKGETSIZE, &blocks) < 0)
			err(1, "Unable to determine block count");	

		return (off_t)blocks * 512;
		#endif

	}

	return -1;
}

static void parse_focus(const unsigned char *data)
{

	if (options.verbose) {
		fputs("Found focus/zip partition\n", stdout);
	}

	int pcount = data[15];

	const unsigned char *name_ptr = data + 512 + 0x20;
	const unsigned char *size_ptr = data + 0x20;
	for (int i = 0; i < pcount; ++i)
	{
		struct file_info f;

		std::string name(name_ptr, name_ptr + 0x20);
		while (!name.empty() && name.back() == 0x00) name.pop_back();

		unsigned start = read32(size_ptr + 0);
		unsigned count = read32(size_ptr + 4);

		if (options.verbose) {
			printf("%d: %-20s %8u %8u\n", i + 1, name.c_str(), start, count);
		}

		f.name = std::move(name);
		f.start =  start * 512;
		f.size = count * 512;

		files.emplace_back(std::move(f));

		name_ptr += 0x20;
		size_ptr += 0x10;
	}
}

#if 0
static bool is_vulcan(const unsigned char *data)
{
	return data[0] == 0xae && data[1] == 0xae;
}
// untested / unconfirmed
static void parse_vulcan(const unsigned char *data)
{
	unsigned start = 1;
	data += 0x100;
	for (unsigned i = 0; i < 15; ++i, data += 16) {
		int flags = data[6];
		unsigned count = read16(data + 3);
		if (flags & 0x20) {
			char tmp[10];
			std::transform(data + 7, data + 16, tmp, [](uint8t_t c){ return c & 0x7f });

			std::string name(tmp, tmp+10);
			while (!name.empty() && name.back() == 0x00) name.pop_back();

			struct file_info f;
			f.name = std::move(name);
			f.start = start * 512;
			f.size = count * 512;
		}
		start += count;
	}
}
#endif


static int setup(const char *path)
{
	unsigned char buffer[512 * 3];

	fd = open(path, options.rw ? O_RDWR : O_RDONLY);
	if (fd < 0) err(1, "Unable to open %s", path);


	if (read(fd, buffer, sizeof(buffer)) < sizeof(buffer)) err(1, "Unable to read %s", path);

	if (is_focus(buffer) || is_zip(buffer)) {
		parse_focus(buffer);
		return 0;
	}

	if (is_microdrive(buffer)) {
		parse_microdrive(buffer);
		return 0;
	}

	close(fd);
	errx(1, "Unknown partition type.");
}


static int part_open(const char *path, struct fuse_file_info *fi)
{
	const std::string spath(path + 1);
	auto iter = std::find(files.begin(), files.end(), spath);
	if (iter == files.end()) return -ENOENT;

	return 0;
}

static int part_getattr(const char *path, struct stat *stbuf)
{
	const std::string spath(path + 1);

	memset(stbuf, 0, sizeof(*stbuf));
	if (spath.empty()) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2 + files.size();
		return 0;
	}
	auto iter = std::find(files.begin(), files.end(), spath);
	if (iter == files.end()) return -ENOENT;

	const file_info &f = *iter;

	stbuf->st_mode = S_IFREG | 0444;
	stbuf->st_nlink = 1;
	stbuf->st_size = f.size;
	return 0;
}

static int part_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	const std::string spath(path + 1);

    if (!spath.empty())
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for(const auto &f : files)
	    filler(buf, f.name.c_str(), NULL, 0);

    return 0;
}

static int part_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	const std::string spath(path + 1);
	ssize_t ok;

	auto iter = std::find(files.begin(), files.end(), spath);
	if (iter == files.end()) return -ENOENT;

	const file_info &f = *iter;
	if (offset >= f.size) return 0;
	if (offset + size > f.size) size = f.size - offset;

	ok = pread(fd, buf, size, f.start + offset);
	if (ok < 0) return -errno;
	return ok;
}

static int part_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	const std::string spath(path + 1);
	ssize_t ok;

	auto iter = std::find(files.begin(), files.end(), spath);
	if (iter == files.end()) return -ENOENT;

	const file_info &f = *iter;
	if (offset >= f.size) return -ENOSPC;
	if (offset + size > f.size) size = f.size - offset;

	ok = pwrite(fd, buf, size, f.start + offset);
	if (ok < 0) return -errno;
	return ok;
}


static struct fuse_operations focus_filesystem_operations = {
    .getattr = part_getattr,
    .open    = part_open,
    .read    = part_read,
    .write   = part_write,
    .readdir = part_readdir,
};

void help(int exitvalue)
{
	fputs(
		"focus_mounter [-oro] [-v] filename-or-device [mountpoint]\n"
		"    -orw                   read/write\n"
		"    -v   --verbose         be verbose\n"
		, stdout
	);
	// fuse_cmdline_help();
	exit(exitvalue);
}

#ifdef __APPLE__

std::string make_mount_dir()
{
	std::string path = "/Volumes/Focus";
	int length = path.length();

	if (mkdir(path.c_str(), 0777) == 0 || errno == EACCES) return path;

	for (int i = 1; i < 256; ++i) {
		path.resize(length);
		path += "-";
		path += std::to_string(i);
		if (mkdir(path.c_str(), 0777) == 0 || errno == EACCES) return path;
	}

	errx(1,"Unable to create mountpoint (/Volumes/Focus)");
	return "";
}

#endif

int main(int argc, char **argv)
{
	int ok;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (fuse_opt_parse(&args, &options, option_spec, part_opt_proc) == -1)
		return 1;

	if (!options.filename) help(1);

	if (setup(options.filename) < 0) return 1;

	#ifdef __APPLE__
	if (!options.mountpoint) {
		static std::string mp = make_mount_dir();
		options.mountpoint = mp.c_str();
		fuse_opt_add_arg(&args, mp.c_str());
	}
	#else
	if (!options.mountpoint) help(1);
	#endif

	if (options.verbose)
		printf ("Mounting %s to %s\n", options.filename, options.mountpoint);


    ok = fuse_main(args.argc, args.argv, &focus_filesystem_operations, NULL);

	fuse_opt_free_args(&args);
    close(fd);
    return ok;
}