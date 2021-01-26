#!/usr/bin/env python3

if __name__ == '__main__':
    import pytest
    import sys
    sys.exit(pytest.main([__file__] + sys.argv[1:]))

import subprocess
import os
import sys
import pytest
import stat
import shutil
import filecmp
import errno
from contextlib import contextmanager
from tempfile import NamedTemporaryFile
from util import (wait_for_mount, umount, cleanup, base_cmdline,
                  basename, fuse_test_marker, safe_sleep)
from os.path import join as pjoin

TEST_FILE = __file__

pytestmark = fuse_test_marker()

with open(TEST_FILE, 'rb') as fh:
    TEST_DATA = fh.read()

def name_generator(__ctr=[0]):
    __ctr[0] += 1
    return 'testfile_%d' % __ctr[0]

@pytest.mark.uses_fuse
@pytest.mark.parametrize("debug", (False, True))
def test_unreliablefs(tmpdir, debug):
    mnt_dir = str(tmpdir.mkdir('mnt'))
    src_dir = str(tmpdir.mkdir('src'))

    modules = "-omodules=subdir,subdir={}".format(src_dir)
    cmdline = base_cmdline + [ pjoin(basename, 'build/unreliablefs'), mnt_dir, modules]
    print(cmdline)
    if debug:
        cmdline += [ '-o', 'debug' ]

    mount_process = subprocess.Popen(cmdline)
    try:
        wait_for_mount(mount_process, mnt_dir)

        tst_statvfs(mnt_dir)
        tst_readdir(src_dir, mnt_dir)
        tst_open_read(src_dir, mnt_dir)
        tst_open_write(src_dir, mnt_dir)
        tst_append(src_dir, mnt_dir)
        tst_seek(src_dir, mnt_dir)
        tst_create(mnt_dir)
        tst_passthrough(src_dir, mnt_dir)
        tst_mkdir(mnt_dir)
        tst_rmdir(src_dir, mnt_dir)
        tst_unlink(src_dir, mnt_dir)
        # FIXME
        #tst_symlink(mnt_dir)
        if os.getuid() == 0:
            tst_chown(mnt_dir)
        tst_link(mnt_dir)
        tst_truncate_path(mnt_dir)
        # FIXME
        #tst_truncate_fd(mnt_dir)
        tst_open_unlink(mnt_dir)
    except:
        cleanup(mount_process, mnt_dir)
        raise
    else:
        umount(mount_process, mnt_dir)

@contextmanager
def os_open(name, flags):
    fd = os.open(name, flags)
    try:
        yield fd
    finally:
        os.close(fd)

def os_create(name):
    os.close(os.open(name, os.O_CREAT | os.O_RDWR))

def tst_unlink(src_dir, mnt_dir):
    name = name_generator()
    fullname = mnt_dir + "/" + name
    with open(pjoin(src_dir, name), 'wb') as fh:
        fh.write(b'hello')
    assert name in os.listdir(mnt_dir)
    os.unlink(fullname)
    with pytest.raises(OSError) as exc_info:
        os.stat(fullname)
    assert exc_info.value.errno == errno.ENOENT
    assert name not in os.listdir(mnt_dir)
    assert name not in os.listdir(src_dir)

def tst_mkdir(mnt_dir):
    dirname = name_generator()
    fullname = mnt_dir + "/" + dirname
    os.mkdir(fullname)
    fstat = os.stat(fullname)
    assert stat.S_ISDIR(fstat.st_mode)
    assert os.listdir(fullname) ==  []
    assert fstat.st_nlink in (1,2)
    assert dirname in os.listdir(mnt_dir)

def tst_rmdir(src_dir, mnt_dir):
    name = name_generator()
    fullname = mnt_dir + "/" + name
    os.mkdir(pjoin(src_dir, name))
    assert name in os.listdir(mnt_dir)
    os.rmdir(fullname)
    with pytest.raises(OSError) as exc_info:
        os.stat(fullname)
    assert exc_info.value.errno == errno.ENOENT
    assert name not in os.listdir(mnt_dir)
    assert name not in os.listdir(src_dir)

def tst_symlink(mnt_dir):
    linkname = name_generator()
    fullname = os.path.join(mnt_dir, linkname)
    os.symlink("/imaginary/dest", fullname)
    fstat = os.lstat(fullname)
    assert stat.S_ISLNK(fstat.st_mode)
    assert os.readlink(fullname) == "/imaginary/dest"
    assert fstat.st_nlink == 1
    assert linkname in os.listdir(mnt_dir)

def tst_create(mnt_dir):
    name = name_generator()
    fullname = pjoin(mnt_dir, name)
    with pytest.raises(OSError) as exc_info:
        os.stat(fullname)
    assert exc_info.value.errno == errno.ENOENT
    assert name not in os.listdir(mnt_dir)

    fd = os.open(fullname, os.O_CREAT | os.O_RDWR)
    os.close(fd)

    assert name in os.listdir(mnt_dir)
    fstat = os.lstat(fullname)
    assert stat.S_ISREG(fstat.st_mode)
    assert fstat.st_nlink == 1
    assert fstat.st_size == 0

def tst_chown(mnt_dir):
    filename = pjoin(mnt_dir, name_generator())
    os.mkdir(filename)
    fstat = os.lstat(filename)
    uid = fstat.st_uid
    gid = fstat.st_gid

    uid_new = uid + 1
    os.chown(filename, uid_new, -1)
    fstat = os.lstat(filename)
    assert fstat.st_uid == uid_new
    assert fstat.st_gid == gid

    gid_new = gid + 1
    os.chown(filename, -1, gid_new)
    fstat = os.lstat(filename)
    assert fstat.st_uid == uid_new
    assert fstat.st_gid == gid_new

def tst_open_read(src_dir, mnt_dir):
    name = name_generator()
    with open(pjoin(src_dir, name), 'wb') as fh_out, \
         open(TEST_FILE, 'rb') as fh_in:
        shutil.copyfileobj(fh_in, fh_out)
    assert len(os.listdir(mnt_dir)) == 1

    assert filecmp.cmp(pjoin(mnt_dir, name), TEST_FILE, False)

def tst_open_write(src_dir, mnt_dir):
    name = name_generator()
    content = b'AABBCC'
    fd = os.open(pjoin(src_dir, name),
                 os.O_CREAT | os.O_RDWR)
    os.write(fd, content)
    os.close(fd)

    fullname = pjoin(mnt_dir, name)
    with open(fullname, 'rb') as fh:
        assert fh.read() == content

def tst_append(src_dir, mnt_dir):
    name = name_generator()
    os_create(pjoin(src_dir, name))
    fullname = pjoin(mnt_dir, name)
    with os_open(fullname, os.O_WRONLY) as fd:
        os.write(fd, b'foo\n')
    with os_open(fullname, os.O_WRONLY|os.O_APPEND) as fd:
        os.write(fd, b'bar\n')

    with open(fullname, 'rb') as fh:
        assert fh.read() == b'foo\nbar\n'

def tst_seek(src_dir, mnt_dir):
    name = name_generator()
    os_create(pjoin(src_dir, name))
    fullname = pjoin(mnt_dir, name)
    with os_open(fullname, os.O_WRONLY) as fd:
        os.lseek(fd, 1, os.SEEK_SET)
        os.write(fd, b'foobar\n')
    with os_open(fullname, os.O_WRONLY) as fd:
        os.lseek(fd, 4, os.SEEK_SET)
        os.write(fd, b'com')

    with open(fullname, 'rb') as fh:
        assert fh.read() == b'\0foocom\n'

def tst_open_unlink(mnt_dir):
    name = pjoin(mnt_dir, name_generator())
    data1 = b'foo'
    data2 = b'bar'
    fullname = pjoin(mnt_dir, name)
    with open(fullname, 'wb+', buffering=0) as fh:
        fh.write(data1)
        os.unlink(fullname)
        with pytest.raises(OSError) as exc_info:
            os.stat(fullname)
            assert exc_info.value.errno == errno.ENOENT
        assert name not in os.listdir(mnt_dir)
        fh.write(data2)
        fh.seek(0)
        # FIXME
        #assert fh.read() == data1+data2

def tst_statvfs(mnt_dir):
    os.statvfs(mnt_dir)

def tst_link(mnt_dir):
    name1 = pjoin(mnt_dir, name_generator())
    name2 = pjoin(mnt_dir, name_generator())
    shutil.copyfile(TEST_FILE, name1)
    assert filecmp.cmp(name1, TEST_FILE, False)

    fstat1 = os.lstat(name1)
    assert fstat1.st_nlink == 1

    os.link(name1, name2)

    fstat1 = os.lstat(name1)
    fstat2 = os.lstat(name2)
    for attr in ('st_mode', 'st_dev', 'st_uid', 'st_gid',
                 'st_size', 'st_atime', 'st_mtime', 'st_ctime'):
        assert getattr(fstat1, attr) == getattr(fstat2, attr)
    assert os.path.basename(name2) in os.listdir(mnt_dir)
    assert filecmp.cmp(name1, name2, False)

    os.unlink(name2)

    assert os.path.basename(name2) not in os.listdir(mnt_dir)
    with pytest.raises(FileNotFoundError):
        os.lstat(name2)

    os.unlink(name1)

def tst_readdir(src_dir, mnt_dir):
    newdir = name_generator()
    src_newdir = pjoin(src_dir, newdir)
    mnt_newdir = pjoin(mnt_dir, newdir)
    file_ = src_newdir + "/" + name_generator()
    subdir = src_newdir + "/" + name_generator()
    subfile = subdir + "/" + name_generator()

    os.mkdir(src_newdir)
    shutil.copyfile(TEST_FILE, file_)
    os.mkdir(subdir)
    shutil.copyfile(TEST_FILE, subfile)

    listdir_is = os.listdir(mnt_newdir)
    listdir_is.sort()
    listdir_should = [ os.path.basename(file_), os.path.basename(subdir) ]
    listdir_should.sort()
    assert listdir_is == listdir_should

    os.unlink(file_)
    os.unlink(subfile)
    os.rmdir(subdir)
    os.rmdir(src_newdir)

def tst_truncate_path(mnt_dir):
    assert len(TEST_DATA) > 1024

    filename = pjoin(mnt_dir, name_generator())
    with open(filename, 'wb') as fh:
        fh.write(TEST_DATA)

    fstat = os.stat(filename)
    size = fstat.st_size
    assert size == len(TEST_DATA)

    # Add zeros at the end
    os.truncate(filename, size + 1024)
    assert os.stat(filename).st_size == size + 1024
    with open(filename, 'rb') as fh:
        assert fh.read(size) == TEST_DATA
        assert fh.read(1025) == b'\0' * 1024

    # Truncate data
    os.truncate(filename, size - 1024)
    assert os.stat(filename).st_size == size - 1024
    with open(filename, 'rb') as fh:
        assert fh.read(size) == TEST_DATA[:size-1024]

    os.unlink(filename)

def tst_truncate_fd(mnt_dir):
    assert len(TEST_DATA) > 1024
    with NamedTemporaryFile('w+b', 0, dir=mnt_dir) as fh:
        fd = fh.fileno()
        fh.write(TEST_DATA)
        fstat = os.fstat(fd)
        size = fstat.st_size
        assert size == len(TEST_DATA)

        # Add zeros at the end
        os.ftruncate(fd, size + 1024)
        assert os.fstat(fd).st_size == size + 1024
        fh.seek(0)
        assert fh.read(size) == TEST_DATA
        assert fh.read(1025) == b'\0' * 1024

        # Truncate data
        os.ftruncate(fd, size - 1024)
        assert os.fstat(fd).st_size == size - 1024
        fh.seek(0)
        assert fh.read(size) == TEST_DATA[:size-1024]

def tst_passthrough(src_dir, mnt_dir):
    name = name_generator()
    src_name = pjoin(src_dir, name)
    mnt_name = pjoin(src_dir, name)
    assert name not in os.listdir(src_dir)
    assert name not in os.listdir(mnt_dir)
    with open(src_name, 'w') as fh:
        fh.write('Hello, world')
    assert name in os.listdir(src_dir)
    assert name in os.listdir(mnt_dir)
    assert os.stat(src_name) == os.stat(mnt_name)

    name = name_generator()
    src_name = pjoin(src_dir, name)
    mnt_name = pjoin(src_dir, name)
    assert name not in os.listdir(src_dir)
    assert name not in os.listdir(mnt_dir)
    with open(mnt_name, 'w') as fh:
        fh.write('Hello, world')
    assert name in os.listdir(src_dir)
    assert name in os.listdir(mnt_dir)
    assert os.stat(src_name) == os.stat(mnt_name)
