/*
 * Copyright 2014-2019, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * mmap.c -- mmap utilities
 */

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "file.h"
#include "mmap.h"
#include "sys_util.h"
#include "os.h"

/*
 * util_map -- memory map a file
 *
 * This is just a convenience function that calls mmap() with the
 * appropriate arguments and includes our trace points.
 */
void *
util_map(int fd, size_t len, int flags, int rdonly, size_t req_align,
	int *map_sync)
{
	LOG(3, "fd %d len %zu flags %d rdonly %d req_align %zu map_sync %p",
			fd, len, flags, rdonly, req_align, map_sync);

	void *base;
	void *addr = util_map_hint(len, req_align);
	if (addr == MAP_FAILED) {
		ERR("cannot find a contiguous region of given size");
		return NULL;
	}

	if (req_align)
		ASSERTeq((uintptr_t)addr % req_align, 0);

	int proto = rdonly ? PROT_READ : PROT_READ|PROT_WRITE;
	base = util_map_sync(addr, len, proto, flags, fd, 0, map_sync);
	if (base == MAP_FAILED) {
		ERR("!mmap %zu bytes", len);
		return NULL;
	}

	LOG(3, "mapped at %p", base);

	return base;
}

/*
 * util_unmap -- unmap a file
 *
 * This is just a convenience function that calls munmap() with the
 * appropriate arguments and includes our trace points.
 */
int
util_unmap(void *addr, size_t len)
{
	LOG(3, "addr %p len %zu", addr, len);

/*
 * XXX Workaround for https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=169608
 */
#ifdef __FreeBSD__
	if (!IS_PAGE_ALIGNED((uintptr_t)addr)) {
		errno = EINVAL;
		ERR("!munmap");
		return -1;
	}
#endif
	int retval = munmap(addr, len);
	if (retval < 0)
		ERR("!munmap");

	return retval;
}

/*
 * chattr -- (internal) set file attributes
 */
static void
chattr(int fd, int set, int clear)
{
	int attr;

	if (ioctl(fd, FS_IOC_GETFLAGS, &attr) < 0) {
		LOG(3, "!ioctl(FS_IOC_GETFLAGS) failed");
		return;
	}

	attr |= set;
	attr &= ~clear;

	if (ioctl(fd, FS_IOC_SETFLAGS, &attr) < 0) {
		LOG(3, "!ioctl(FS_IOC_SETFLAGS) failed");
		return;
	}
}

/*
 * util_map_tmpfile -- reserve space in an unlinked file and memory-map it
 *
 * size must be multiple of page size.
 */
void *
util_map_tmpfile(const char *dir, size_t size, size_t req_align)
{
	int oerrno;

	if (((os_off_t)size) < 0) {
		ERR("invalid size (%zu) for os_off_t", size);
		errno = EFBIG;
		return NULL;
	}

	int fd = util_tmpfile(dir, OS_DIR_SEP_STR "vmem.XXXXXX", O_EXCL);
	if (fd == -1) {
		LOG(2, "cannot create temporary file in dir %s", dir);
		goto err;
	}

	chattr(fd, FS_NOCOW_FL, 0);

	if ((errno = os_posix_fallocate(fd, 0, (os_off_t)size)) != 0) {
		ERR("!posix_fallocate");
		goto err;
	}

	void *base;
	if ((base = util_map(fd, size, MAP_SHARED,
			0, req_align, NULL)) == NULL) {
		LOG(2, "cannot mmap temporary file");
		goto err;
	}

	(void) os_close(fd);
	return base;

err:
	oerrno = errno;
	if (fd != -1)
		(void) os_close(fd);
	errno = oerrno;
	return NULL;
}
