#define _GNU_SOURCE
#include "atto/host_fs_broker.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void put_file(const char *p, const char *s) {
    int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600); assert(fd>=0);
    assert(write(fd,s,strlen(s))==(ssize_t)strlen(s)); assert(close(fd)==0);
}
static void expect_file(const char *p,const char *expected) {
    char data[64]={0}; int fd=open(p,O_RDONLY|O_CLOEXEC); assert(fd>=0);
    ssize_t n=read(fd,data,sizeof(data)-1); assert(n==(ssize_t)strlen(expected));
    assert(memcmp(data,expected,(size_t)n)==0); assert(close(fd)==0);
}
static void join_path(char *out, size_t capacity, const char *parent, const char *leaf) {
    const size_t n = strlen(parent), m = strlen(leaf);
    assert(n + 1u + m + 1u <= capacity);
    memcpy(out, parent, n);
    out[n] = '/';
    memcpy(out + n + 1u, leaf, m + 1u);
}
int main(void) {
    char base[]="/tmp/atto-fs-broker-XXXXXX"; assert(mkdtemp(base));
    char root[512], data[512], out[512], input[512], copy[512], outside[512], linkpath[512], alias[512], restricted[512];
    join_path(root,sizeof(root),base,"root"); assert(mkdir(root,0700)==0);
    join_path(data,sizeof(data),root,"data"); assert(mkdir(data,0700)==0);
    join_path(out,sizeof(out),root,"out"); assert(mkdir(out,0700)==0);
    join_path(input,sizeof(input),data,"input.txt"); put_file(input,"secret-data");
    join_path(copy,sizeof(copy),out,"copy.txt");
    join_path(outside,sizeof(outside),base,"outside.txt"); put_file(outside,"outside");
    join_path(linkpath,sizeof(linkpath),root,"data/escape.txt"); assert(symlink(outside,linkpath)==0);
    join_path(alias,sizeof(alias),root,"data/alias.txt"); assert(link(outside,alias)==0);
    join_path(restricted,sizeof(restricted),root,"data/blocked.txt"); put_file(restricted,"blocked");

    atto_fs_policy policy={"data", "out", "data/blocked.txt", "out/locked.txt",42u,17u};
    atto_fs_broker broker; assert(atto_fs_broker_init(&broker,root,&policy)==0);
    unsigned char output[128]={0}; size_t nread=999;
    assert(atto_fs_broker_read(&broker,42,17,"data/input.txt",output,sizeof(output),&nread)==0);
    assert(nread==11 && memcmp(output,"secret-data",11)==0);
    assert(atto_fs_broker_write(&broker,42,17,"out/copy.txt",output,nread)==0);
    expect_file(copy,"secret-data");
    /* Cross-tenant/epoch attempts fail before any I/O. */
    assert(atto_fs_broker_read(&broker,43,17,"data/input.txt",output,sizeof(output),&nread)==-EACCES && nread==0);
    assert(atto_fs_broker_write(&broker,42,18,"out/copy.txt",(const unsigned char*)"BAD",3)==-EACCES);
    expect_file(copy,"secret-data");
    /* Narrow read vs write scopes; deny override; non-prefix match. */
    assert(atto_fs_broker_read(&broker,42,17,"out/copy.txt",output,sizeof(output),&nread)==-EACCES);
    assert(atto_fs_broker_write(&broker,42,17,"data/input.txt",(const unsigned char*)"BAD",3)==-EACCES);
    assert(atto_fs_broker_read(&broker,42,17,"data/blocked.txt",output,sizeof(output),&nread)==-EACCES);
    assert(atto_fs_broker_write(&broker,42,17,"out/locked.txt",(const unsigned char*)"BAD",3)==-EACCES);
    assert(atto_fs_broker_read(&broker,42,17,"data2/file",output,sizeof(output),&nread)==-EACCES);
    /* Escape probes must never reach outside root. */
    assert(atto_fs_broker_read(&broker,42,17,"data/escape.txt",output,sizeof(output),&nread)!=0);
    assert(atto_fs_broker_read(&broker,42,17,"data/alias.txt",output,sizeof(output),&nread)!=0);
    assert(atto_fs_broker_read(&broker,42,17,"data/../outside.txt",output,sizeof(output),&nread)==-EINVAL);
    assert(atto_fs_broker_read(&broker,42,17,"/etc/passwd",output,sizeof(output),&nread)==-EINVAL);
    assert(atto_fs_broker_write(&broker,42,17,"out/../outside.txt",(const unsigned char*)"BAD",3)==-EINVAL);
    expect_file(outside,"outside");
    /* A forbidden symlink at the write target must not modify its target. */
    char out_link[512]; join_path(out_link,sizeof(out_link),root,"out/escape.txt");
    assert(symlink(outside,out_link)==0);
    assert(atto_fs_broker_write(&broker,42,17,"out/escape.txt",(const unsigned char*)"BAD",3)!=0);
    expect_file(outside,"outside");
    /* Atomic replacement does not mutate old inode contents outside the root. */
    char external_copy[512]; join_path(external_copy,sizeof(external_copy),base,"old-inode.txt");
    assert(link(copy,external_copy)==0);
    assert(atto_fs_broker_write(&broker,42,17,"out/copy.txt",(const unsigned char*)"NEW",3)==-EACCES);
    expect_file(copy,"secret-data"); expect_file(external_copy,"secret-data");
    assert(unlink(external_copy)==0);
    assert(atto_fs_broker_write(&broker,42,17,"out/copy.txt",(const unsigned char*)"NEW",3)==0);
    expect_file(copy,"NEW");
    /* Root descriptor is pinned after rename; replacement symlink is ignored. */
    char renamed[512]; join_path(renamed,sizeof(renamed),base,"renamed");
    assert(rename(root,renamed)==0);
    assert(symlink(base,root)==0);
    assert(atto_fs_broker_read(&broker,42,17,"data/input.txt",output,sizeof(output),&nread)==0);
    atto_fs_broker_close(&broker);
    assert(atto_fs_broker_read(&broker,42,17,"data/input.txt",output,sizeof(output),&nread)==-EACCES);
    atto_fs_broker new_broker;
    assert(atto_fs_broker_init(&new_broker,root,&policy)!=0);
    puts("ATTO_HOST_FS_BROKER_HOST_ONLY_TEST_PASS");
    return 0;
}
