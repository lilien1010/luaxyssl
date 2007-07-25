/*
* lxyssl.c
* xyssl library binding for Lua 5.1
* Copyright 2007 Gary Ng<linux@garyng.com>
* This code can be distributed under the LGPL license
*/

#include <stdio.h>
#include <memory.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "xyssl/net.h"
#include "xyssl/ssl.h"
#include "xyssl/havege.h"
#include "xyssl/certs.h"
#include "xyssl/x509.h"
#include "xyssl/sha1.h"
#include "xyssl/sha2.h"
#include "xyssl/md5.h"
#include "xyssl/aes.h"
#include "xyssl/arc4.h"

/*
 * Computing a safe DH-1024 prime takes ages, so it's faster
 * to use a precomputed value (provided below as an example).
 * Run the dh_genprime program to generate an acceptable P.
 */
char *default_dhm_P = 
    "E4004C1F94182000103D883A448B3F80" \
    "2CE4B44A83301270002C20D0321CFD00" \
    "11CCEF784C26A400F43DFB901BCA7538" \
    "F2C6B176001CF5A0FD16D2C48B1D0C1C" \
    "F6AC8E1DA6BCC3B4E1F96B0564965300" \
    "FFA1D0B601EB2800F489AA512C4B248C" \
    "01F76949A60BB7F00A40B1EAB64BDD48" \
    "E8A700D60B7F1200FA8E77B0A979DABF";

char *default_dhm_G = "4";

/*
 * sorted by order of preference
 */
int my_preferred_ciphers[] =
{
    SSL3_RSA_RC4_128_MD5,
    SSL3_RSA_RC4_128_SHA,
    TLS1_EDH_RSA_AES_256_SHA,
    SSL3_EDH_RSA_DES_168_SHA,
    TLS1_RSA_AES_256_SHA,
    SSL3_RSA_DES_168_SHA,
    0
};

enum {
    MD5,
    SHA1,
    SHA2,
    HMAC_MD5,
    HMAC_SHA1,
    HMAC_SHA2,
};

typedef struct {
 ssl_context ssl;
 x509_cert cacert;
 x509_cert mycert;
 rsa_context mykey;
 unsigned char session_table[SSL_SESSION_TBL_LEN];
 double timeout;
 int last_send_size;
 char *peer_cn;
 int closed;
} xyssl_context;

#define EXPORT_HASH_FUNCTIONS
#if 0
#define EXPORT_SHA2
#endif

typedef struct {
    union {
        md5_context md5;
        sha1_context sha1;
#ifdef EXPORT_SHA2
        sha2_context sha2;
#endif
    } eng;
    void (*starts)(void *);
    void (*update)(void *, unsigned char*, int);
    void (*finish)(void *, unsigned char*);
    int hash_size;
    int id;
} hash_context;


#define MYVERSION	"XySSL for " LUA_VERSION " 0.1"
#define MYTYPE		"XySSL SSL object"
#define MYHASH      "XySSL Hash object"
#define MYAES       "XySSL AES object"
#define MYRC4       "XySSL RC4 object"

havege_state hs;

static int Pselect(int fd, double t, int w)
{
    struct timeval tm;
    fd_set set;
    FD_ZERO(&set);

    if (t >= 0.0) {
        tm.tv_sec = (int) t;
        tm.tv_usec = (int) ((t - tm.tv_sec)*1000);
    }

    FD_SET(fd, &set);
    if (w) {
        return select(fd+1, NULL, &set, NULL, t < 0 ? NULL : &tm);
    }
    else {
        return select(fd+1, &set, NULL, NULL, t < 0 ? NULL : &tm);
    }
}

static xyssl_context *Pget(lua_State *L, int i)
{
 if (luaL_checkudata(L,i,MYTYPE)==NULL) luaL_typerror(L,i,MYTYPE);
 return lua_touserdata(L,i);
}

static int Psetfd(lua_State *L)		/** setfd(r[,w]) */
{
 xyssl_context *xyssl=Pget(L,1);
 int read_fd = luaL_checknumber(L,2);
 int write_fd = luaL_optinteger(L,3,read_fd);
 ssl_context *ssl=&xyssl->ssl;

 ssl_set_io_files( ssl, read_fd, write_fd );
}

static int Laes(lua_State *L)
{
 int klen;
 const unsigned char *key = luaL_checklstring(L, 1, &klen);
 int bits = luaL_optinteger(L, 2, 128);
 aes_context *aes = lua_newuserdata(L,sizeof(aes_context));

 if (klen*8 != bits) {
    lua_pop(L, 1);
    luaL_error(L,"xyssl.aes: key not long enough for selected bits length");
 }
 luaL_getmetatable(L,MYAES);
 lua_setmetatable(L,-2);
 aes_set_key(aes, (unsigned char *)key, bits);

 return 1;
}

static int Lrc4(lua_State *L)
{
 int klen;
 const unsigned char *key = luaL_checklstring(L, 1, &klen);
 arc4_context *rc4 = lua_newuserdata(L,sizeof(arc4_context));
 arc4_setup(rc4, (unsigned char *)key, klen);
 luaL_getmetatable(L,MYRC4);
 lua_setmetatable(L,-2);

 return 1;
}

#ifdef EXPORT_HASH_FUNCTIONS
static int Lhash(lua_State *L)
{
 const char *type = luaL_checkstring(L,1);
 int klen;
 const unsigned char *key = luaL_optlstring(L, 2, NULL, &klen);
 hash_context *obj = lua_newuserdata(L,sizeof(hash_context));
 
 if (!klen) {
     if (memcmp(type,"md5",3)==0) {
        md5_starts(&obj->eng.md5);
        obj->id = MD5;
        obj->hash_size = 16;
        obj->starts = md5_starts;
        obj->update = md5_update;
        obj->finish = md5_finish;
     } else if (memcmp(type,"sha1",4)==0) {
        sha1_starts(&obj->eng.sha1);
        obj->id = SHA1;
        obj->hash_size = 20;
        obj->starts = sha1_starts;
        obj->update = sha1_update;
        obj->finish = sha1_finish;
#ifdef EXPORT_SHA2
     } else if (memcmp(type,"sha2",4)==0) {
        sha2_starts(&obj->eng.sha2,0);
        obj->id = SHA2;
        obj->hash_size = 32;
        obj->starts = sha2_starts;
        obj->update = sha2_update;
        obj->finish = sha2_finish;
#endif
     } else {
        lua_pop(L, 1);
        luaL_error(L,"xyssl.hash: unknown hash function");
     }
 }
 else {
     if (memcmp(type,"hmac-md5",8)==0) {
        md5_hmac_starts(&obj->eng.md5, (unsigned char *)key, klen);
        obj->id = HMAC_MD5;
        obj->hash_size = 16;
        obj->starts = md5_starts;
        obj->update = md5_hmac_update;
        obj->finish = md5_hmac_finish;
     } else if (memcmp(type,"hmac-sha1",9)==0) {
        sha1_hmac_starts(&obj->eng.sha1, (unsigned char *)key, klen);
        obj->id = HMAC_SHA1;
        obj->hash_size = 20;
        obj->starts = sha1_starts;
        obj->update = sha1_hmac_update;
        obj->finish = sha1_hmac_finish;
#ifdef EXPORT_SHA2
     } else if (memcmp(type,"hmac-sha2",9)==0) {
        sha2_hmac_starts(&obj->eng.sha2, 0, (unsigned char *)key, klen);
        obj->id = HMAC_SHA2;
        obj->hash_size = 32;
        obj->starts = sha2_starts;
        obj->update = sha2_hmac_update;
        obj->finish = sha2_hmac_finish;
#endif
     } else {
        lua_pop(L, 1);
        luaL_error(L,"xyssl.hash: unknown hmac function");
     }
 }
 luaL_getmetatable(L,MYHASH);
 lua_setmetatable(L,-2);

 return 1;
}

static hash_context *Pget_hash(lua_State *L, int i)
{
 if (luaL_checkudata(L,i,MYHASH)==NULL) luaL_typerror(L,i,MYHASH);
 return lua_touserdata(L,i);
}

static aes_context *Pget_aes(lua_State *L, int i)
{
 if (luaL_checkudata(L,i,MYAES)==NULL) luaL_typerror(L,i,MYAES);
 return lua_touserdata(L,i);
}

static arc4_context *Pget_rc4(lua_State *L, int i)
{
 if (luaL_checkudata(L,i,MYRC4)==NULL) luaL_typerror(L,i,MYRC4);
 return lua_touserdata(L,i);
}

static int Lhash_reset(lua_State *L)
{
 hash_context *obj=Pget_hash(L,1);
 obj->starts(&obj->eng);
 if (obj->id == HMAC_MD5) {
    char *inpad = obj->eng.md5.ipad;
    int len = sizeof(obj->eng.md5.ipad);
    obj->update(&obj->eng, inpad, len);
 }
 else if (obj->id == HMAC_SHA1) {
    char *inpad = obj->eng.sha1.ipad;
    int len = sizeof(obj->eng.sha1.ipad);
    obj->update(&obj->eng, inpad, len);
 } 
#ifdef EXPORT_SHA2
 else if (obj->id == HMAC_SHA2) {
    char *inpad = obj->eng.sha2.ipad;
    int len = sizeof(obj->eng.sha2.ipad);
    obj->update(&obj->eng, inpad, len);
 }
#endif

 lua_pushvalue(L, 1);
 return 1;
}

static int Laes_encrypt(lua_State *L)
{
 aes_context *obj=Pget_aes(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 int i;
 luaL_Buffer B;

 if (len % 16) luaL_error(L,"xyssl.aes: data must be in 16 byte multiple");
 luaL_buffinit(L, &B);
 for(i = 0; i < len; i+=16) {
    unsigned char temp[16];

    aes_encrypt(obj, (unsigned char *)&data[i], temp);
    luaL_addlstring(&B, temp, 16);
 }
 luaL_pushresult(&B);

 return 1;
}

static int Laes_decrypt(lua_State *L)
{
 aes_context *obj=Pget_aes(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 int i;
 luaL_Buffer B;

 if (len % 16) luaL_error(L,"xyssl.aes: data must be in 16 byte multiple");
 luaL_buffinit(L, &B);
 for(i = 0; i < len; i+=16) {
    unsigned char temp[16];

    aes_decrypt(obj, (unsigned char *)&data[i], temp);
    luaL_addlstring(&B, temp, 16);
 }
 luaL_pushresult(&B);

 return 1;
}

static int Lrc4_crypt(lua_State *L)
{
 arc4_context *obj=Pget_rc4(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 luaL_Buffer B;
 unsigned char temp[256];
 int t_size = sizeof(temp);
 int i;

 luaL_buffinit(L, &B);
 for(i = 0; i < len - t_size; i+=sizeof(temp)) {
    memcpy(temp, &data[i], sizeof(temp));
    arc4_crypt(obj, temp, sizeof(temp));
    luaL_addlstring(&B, temp, sizeof(temp));
 }
 if (i < len) {
    int j = len - i;
    memcpy(temp, &data[i], j);
    arc4_crypt(obj, temp, j);
    luaL_addlstring(&B, temp, j);
 }
 luaL_pushresult(&B);

 return 1;
}

static int Laes_cbc_encrypt(lua_State *L)
{
 aes_context *obj=Pget_aes(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 int iv_len;
 const char *IV = luaL_checklstring(L, 3, &iv_len);
 int i=0;
 luaL_Buffer B;
 unsigned char iv[16];
 unsigned char temp[256];
 int t_size = sizeof(temp);

 if (len % 16) luaL_error(L,"xyssl.aes: data must be in 16 byte multiple");
 if (iv_len != 16) luaL_error(L,"xyssl.aes: IV must be 16 bytes");

 luaL_buffinit(L, &B);
 memcpy(iv, IV, 16);
 for(i = 0; i < len - t_size; i+=sizeof(temp)) {
    aes_cbc_encrypt(obj, iv, (unsigned char *)&data[i], temp, sizeof(temp));
    luaL_addlstring(&B, temp, sizeof(temp));
 }
 if (i < len) {
    aes_cbc_encrypt(obj, iv, (unsigned char *)&data[i], temp, len - i);
    luaL_addlstring(&B, temp, len - i);
 }
 luaL_pushresult(&B);
 lua_pushlstring(L,iv, 16);

 return 2;
}

static int Laes_cbc_decrypt(lua_State *L)
{
 aes_context *obj=Pget_aes(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 int iv_len;
 const char *IV = luaL_checklstring(L, 3, &iv_len);
 int i;
 luaL_Buffer B;
 unsigned char iv[16];
 unsigned char temp[256];
 int t_size = sizeof(temp);

 if (len % 16) luaL_error(L,"xyssl.aes: data must be in 16 byte multiple");
 if (iv_len != 16) luaL_error(L,"xyssl.aes: IV must be 16 bytes");

 luaL_buffinit(L, &B);
 memcpy(iv, IV, 16);
 for(i = 0; i < len - t_size; i+=sizeof(temp)) {
    aes_cbc_decrypt(obj, iv, (unsigned char *)&data[i], temp, sizeof(temp));
    luaL_addlstring(&B, temp, sizeof(temp));
 }
 if (i < len) {
    aes_cbc_decrypt(obj, iv, (unsigned char *)&data[i], temp, len - i);
    luaL_addlstring(&B, temp, len - i);
 }
 luaL_pushresult(&B);
 lua_pushlstring(L,iv, 16);

 return 2;
}

static int Laes_cfb_encrypt(lua_State *L)
{
 aes_context *obj=Pget_aes(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 int iv_len;
 const char *IV = luaL_checklstring(L, 3, &iv_len);
 int start = luaL_optinteger(L,4,0);
 int i;
 luaL_Buffer B;
 unsigned char iv[16];
 unsigned char temp[256];
 unsigned char *o;

 if (iv_len != 16) luaL_error(L,"xyssl.aes: IV must be 16 bytes");

 luaL_buffinit(L, &B);
 memcpy(iv, IV, 16);
 for(i = 0, o = temp; i < len; i++) {
    if (!start) aes_encrypt(obj, iv, iv);
    iv[start] = *o++ = data[i]^iv[start];
    start = (start + 1)%16;
    if (i%256==255) {
        luaL_addlstring(&B, temp, sizeof(temp));
        o = temp;
    }
 }
 if (o - temp) {
    luaL_addlstring(&B, temp, o - temp);
 }
 luaL_pushresult(&B);
 lua_pushlstring(L,iv, 16);
 lua_pushinteger(L,start);

 return 3;
}

static int Laes_cfb_decrypt(lua_State *L)
{
 aes_context *obj=Pget_aes(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 int iv_len;
 const char *IV = luaL_checklstring(L, 3, &iv_len);
 int start = luaL_optinteger(L,4,0);
 int i;
 luaL_Buffer B;
 unsigned char iv[16];
 unsigned char temp[256];
 unsigned char *o;

 if (iv_len != 16) luaL_error(L,"xyssl.aes: IV must be 16 bytes");

 luaL_buffinit(L, &B);
 memcpy(iv, IV, 16);
 for(i = 0, o = temp; i < len; i++) {
    unsigned char c;
    if (!start) aes_encrypt(obj, iv, iv);
    c = data[i];
    *o++ = c^iv[start];
    iv[start] = c;
    start = (start + 1)%16;
    if (i%256==255) {
        luaL_addlstring(&B, temp, sizeof(temp));
        o = temp;
    }
 }
 if (o - temp) {
    luaL_addlstring(&B, temp, o - temp);
 }
 luaL_pushresult(&B);
 lua_pushlstring(L,iv, 16);
 lua_pushinteger(L,start);

 return 3;
}

static int Lhash_update(lua_State *L)
{
 hash_context *obj=Pget_hash(L,1);
 int len;
 const char *data = luaL_checklstring(L, 2, &len);
 obj->update(&obj->eng, (unsigned char *)data, len);
 lua_pushvalue(L, 1);

 return 1;
}

static int Lhash_digest(lua_State *L)
{
 hash_context *obj=Pget_hash(L,1);
 unsigned char out[64];
 int len;
 const char *data = luaL_optlstring(L, 2, "", &len);
 obj->update(&obj->eng, (unsigned char *)data, len);
 obj->finish(&obj->eng, out);
 
 lua_pushlstring(L,out, obj->hash_size);

 return 1;
}

#endif

static int Lssl(lua_State *L)
{
 int ret;
 int is_server = luaL_optinteger(L,1,0);
 char *dhm_P = (char *)luaL_optstring(L, 2, default_dhm_P);
 char *dhm_G = (char *)luaL_optstring(L, 3, default_dhm_G);
 xyssl_context *xyssl=lua_newuserdata(L,sizeof(xyssl_context));
 ssl_context *ssl = &xyssl->ssl;

 memset(xyssl, 0, sizeof( xyssl_context) );
 xyssl->timeout = 0.1;
 xyssl->last_send_size = -1;

 luaL_getmetatable(L,MYTYPE);
 lua_setmetatable(L,-2);

 ret = ssl_init(ssl,is_server ? 0 : 1);
 if (ret!= 0) {
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_pushnumber(L, ret);
    return 2;
 }

 ssl_set_endpoint( ssl, is_server ? SSL_IS_SERVER : SSL_IS_CLIENT );
 ssl_set_authmode( ssl, SSL_VERIFY_NONE );

 ssl_set_rng_func( ssl, havege_rand, &hs );
 ssl_set_ciphlist( ssl, my_preferred_ciphers);
 #if 0
 ssl_set_ciphlist( ssl, ssl_default_ciphers );
 #endif

 if (is_server) {
    ssl_set_sidtable( ssl, xyssl->session_table );
    ssl_set_dhm_vals( ssl, dhm_P, dhm_G );
 }
 return 1;
}

static int Lconnect(lua_State *L)			/** connect(read_fd[,write_fd]) */
{
 Psetfd(L);
 lua_pushnumber(L, 1);

 return 1;
}

static int Pclose(lua_State *L)			
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 x509_cert *cacert = &xyssl->cacert;
 x509_cert *mycert= &xyssl->mycert;
 rsa_context *rsa = &xyssl->mykey;

 x509_free_cert( cacert );
 x509_free_cert( mycert );
 rsa_free( rsa );

 ssl_close_notify( ssl );
 ssl_free(ssl);
 if (xyssl->peer_cn) {
    free(xyssl->peer_cn);
    xyssl->peer_cn = NULL;
 }
 return 0;
}
static int Lclose(lua_State *L)			/** close(c) */
{
 return Pclose(L);
}

static int Lsend(lua_State *L)		/** send(data) */
{
 int    top = lua_gettop(L);
 size_t size = 0, sent = 0;
 int err = 0;
 int l;
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 int pending = ssl->out_left;
 const char *data = luaL_checklstring(L, 2, &size);
 int start = luaL_optinteger(L,3,1);

 if (ssl->out_uoff && (size != xyssl->last_send_size || start-1 != ssl->out_uoff)) {
    luaL_error(L,"xyssl(send): partial send data in buffer, must use data and return index+1 from previous send");
    }

 if (xyssl->closed) {
    lua_pushnil(L);
    lua_pushstring(L,"nossl");
    lua_pushnumber(L, 0);
    return 3;
 }
 if (1) {
    /* always from start of buffer as it is memorized from last 
     * call
     */
    if (xyssl->timeout <= 0.0 || (err = Pselect(ssl->write_fd, 0, 1)) > 0) {
        err = ssl_write(ssl, (char *)data, size); 
        if (err) {
            xyssl->last_send_size = size;
            sent = ssl->out_uoff ? ssl->out_uoff : 0;
        } else {
            sent = size;
            xyssl->last_send_size = -1;
            }
        }
    else if (err == 0) err = ERR_NET_WOULD_BLOCK;
    else err = ERR_NET_CONN_RESET;
 } else sent = 0;

 if (err!=0) {
    lua_pushnil(L);
    if (err == ERR_NET_WOULD_BLOCK ) lua_pushstring(L, "timeout");
    else if (err == ERR_NET_CONN_RESET) lua_pushstring(L,"closed");
    else if (err == ERR_SSL_PEER_CLOSE_NOTIFY) {
        lua_pushstring(L,"nossl");
        xyssl->closed = 1;
        }
    else lua_pushstring(L, "handshake");
    lua_pushnumber(L, start > sent ? start-1 : sent);
 } else {
    lua_pushnumber(L, sent);
    lua_pushnil(L);
    lua_pushnil(L);
 }

 return lua_gettop(L) - top;
}

static int Lreceive(lua_State *L)		/** receive(cnt) */
{
 int    top = lua_gettop(L);
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 size_t cnt = luaL_checknumber(L,2);
 size_t part_cnt;
 const char *part = luaL_optlstring(L, 3, NULL, &part_cnt);
 size_t len = 0;
 int    ret;
 char   *buf = malloc(cnt);
 luaL_Buffer B;

 if (xyssl->closed) {
    if (buf) free(buf);
    lua_pushnil(L);
    lua_pushstring(L,"nossl");
    lua_pushstring(L, "");
    return 3;
 }
 if (buf) {
     if (ssl->in_offt || xyssl->timeout <= 0.0 || (ret = Pselect(ssl->read_fd, 0, 0)) > 0) {
        len = cnt;
        ret = ssl_read(ssl, buf, &len );
        }
     else if (ret == 0) ret = ERR_NET_WOULD_BLOCK;
     else ret = ERR_NET_CONN_RESET;

     if (ret==0) {
        luaL_buffinit(L, &B);
        luaL_addlstring(&B, part, part_cnt);
        luaL_addlstring(&B, buf, len);
        luaL_pushresult(&B);
        lua_pushnil(L);
        lua_pushnil(L);
     } else {
        lua_pushnil(L);
        if (ret == ERR_NET_WOULD_BLOCK ) lua_pushstring(L, "timeout");
        else if (ret == ERR_NET_CONN_RESET) lua_pushstring(L,"closed");
        else if (ret == ERR_SSL_PEER_CLOSE_NOTIFY) {
            lua_pushstring(L,"nossl");
            xyssl->closed = 1;
            }
        else lua_pushstring(L,"handshake");
        
        luaL_buffinit(L, &B);
        if (part_cnt) luaL_addlstring(&B, part, part_cnt);
        if (len) luaL_addlstring(&B, buf, len);
        luaL_pushresult(&B);
    }
    free(buf);
 } else {
    lua_pushnil(L);
    lua_pushstring(L, "oom");
    lua_pushstring(L, "");
 }
 return lua_gettop(L) - top;
}

static int Lgc(lua_State *L)		/** garbage collect */
{
 return Pclose(L);
}

static int Lkeycert(lua_State *L)		/** set the key/cert to use */
{
 int    top = lua_gettop(L);

 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 x509_cert *cacert = &xyssl->cacert;
 x509_cert *mycert= &xyssl->mycert;
 rsa_context *rsa = &xyssl->mykey;
 int ca_len;
 const char *ca = luaL_optlstring(L, 2, test_ca_crt , &ca_len);
 int cert_len;
 const char *cert = luaL_optlstring(L, 3, ssl->endpoint ? test_srv_crt: NULL, &cert_len);
 int key_len;
 const char *key = luaL_optlstring(L, 4, ssl->endpoint ? test_srv_key: NULL, &key_len);
 int pwd_len;
 const char *pwd = luaL_optlstring(L, 5, NULL, &pwd_len);
 int ret;

 ret = x509_add_certs( cacert, (unsigned char *) ca, ca_len);
 if (ret) {
    lua_pushnil(L);
    lua_pushstring(L,"bad ca");
    lua_pushnumber(L, ret);
    goto exit;
 }
 if (cert) ret = x509_add_certs( mycert, (unsigned char *) cert,cert_len );
 if (ret) {
    lua_pushnil(L);
    lua_pushstring(L,"bad cert");
    lua_pushnumber(L, ret);
    goto free_ca;
 }
 if (key) ret = x509_parse_key(rsa, (unsigned char *) key, key_len, (unsigned char *)pwd, pwd_len);
 if (ret) {
    lua_pushnil(L);
    lua_pushstring(L,"bad rsa key/pwd");
    lua_pushnumber(L, ret);
    goto free_key;
 }

 ssl_set_ca_chain( ssl, cacert, xyssl->peer_cn );
 if (cert) ssl_set_rsa_cert( ssl, mycert, rsa );
 lua_pushnumber(L, 1);
 goto exit;
 
free_key:
 rsa_free( rsa );

free_cert:
 x509_free_cert( mycert );

free_ca:
 x509_free_cert( cacert );
 
exit:

 return lua_gettop(L) - top;
}

static int Lgetfd(lua_State *L)		/** getfd */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 lua_pushnumber(L, ssl->read_fd);
 lua_pushnumber(L, ssl->write_fd);
 return 2;
}

static int Lsetfd(lua_State *L)		/** setfd(r[,w]) */
{
 Psetfd(L);
 lua_pushnumber(L, 1);
 return 1;
}

static int Lauthmode(lua_State *L)		/** authmode(level) */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 int verification = luaL_optinteger(L,2,0);
 int peer_len;
 const char *expected_peer= luaL_optlstring(L, 3, NULL, &peer_len);
 ssl_set_authmode( ssl, verification );
 if (xyssl->peer_cn) free(xyssl->peer_cn);
 if (expected_peer) {
    xyssl->peer_cn = malloc(peer_len+1);
    memcpy(xyssl->peer_cn, expected_peer, peer_len);
    xyssl->peer_cn[peer_len]='\0';
 } else {
    xyssl->peer_cn = NULL;
 }
 if (ssl->ca_chain) ssl_set_ca_chain( ssl, ssl->ca_chain, xyssl->peer_cn );

 return 0;
}

static int Lhandshake(lua_State *L)		/** handshake() */
{
 int ret;
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 if (xyssl->timeout <= 0.0 || (ret = Pselect(ssl->write_fd, xyssl->timeout, 1)) > 0) {
     ret = ssl_handshake( ssl );
 } 
 lua_pushnumber(L, ret);

 return 1;
}

static int Lverify(lua_State *L)		/** verify() */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 int ret = ssl_get_verify_result ( ssl );

 lua_pushnumber(L, ret);

 return 1;
}

static int Lpeer(lua_State *L)		/** peer() */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 if (ssl->peer_cert) {
     char *info = x509_cert_info ( ssl->peer_cert );

     if (info) {
        lua_pushstring(L,info);
        free(info);
     } else lua_pushnil(L);
 } else lua_pushnil(L);

 return 1;
}

static int Lcipher_info(lua_State *L)		/** cipher_info() */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 char *cipher_choosen = ssl_get_cipher_name(ssl);
 if (cipher_choosen) {
    lua_pushstring(L,cipher_choosen);
 } else lua_pushnil(L);

 return 1;
}

static int Lname(lua_State *L)		/** name() */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 if (ssl->own_cert) {
     char *info = x509_cert_info ( ssl->own_cert );

     if (info) {
        lua_pushstring(L,info);
        free(info);
     } else lua_pushnil(L);
 }
 else lua_pushnil(L);

 return 1;
}

static int Lsettimeout(lua_State *L) /** settimeout(sec) **/
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl = &xyssl->ssl;
 double t = luaL_optnumber(L, 2, -1);
 lua_pushnumber(L,xyssl->timeout);
 xyssl->timeout = t;
 if (t < 0.0) {
     net_set_block(ssl->read_fd);
     net_set_block(ssl->write_fd);
 } else {
     net_set_nonblock(ssl->read_fd);
     net_set_nonblock(ssl->write_fd);
 }
 return 1;
}

static int Ldirty(lua_State *L)		/** dirty() */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 lua_pushboolean(L,ssl->in_offt!=NULL);
 return 1;
}

static int Ltostring(lua_State *L)		/** tostring(c) */
{
 xyssl_context *xyssl=Pget(L,1);
 ssl_context *ssl=&xyssl->ssl;
 char s[64];
 sprintf(s,"%s %p",MYTYPE,ssl);
 lua_pushstring(L,s);
 return 1;
}

static int Lrand(lua_State *L)		/** rand(bytes) */
{
 luaL_Buffer B;
 int cnt = luaL_optnumber(L,1,1);
 int i;
 int rem;
 unsigned char buf[256];
 unsigned char *o = buf;

 luaL_buffinit(L, &B);
 for (i = 0; i < cnt; i++) {
    *o++ = havege_rand(&hs);
    if (i % 256 == 255) {
        luaL_addlstring(&B, buf, 256);
        o = buf;
    }
 }
 rem = i % 256;
 if (rem) luaL_addlstring(&B, buf, rem);
 luaL_pushresult(&B);

 return 1;
}

static const luaL_reg R[] =
{
	{ "__tostring",	Ltostring},
	{ "write",  Lsend	},
	{ "send",	Lsend	},
	{ "read",	Lreceive	},
	{ "receive",Lreceive	},
	{ "__gc",	Lgc	},
	{ "close",	Lclose},
	{ "getfd",	Lgetfd},
	{ "setfd",	Lsetfd},
	{ "dirty",Ldirty},
	{ "handshake",Lhandshake},
	{ "authmode",	Lauthmode},
	{ "verify",	Lverify},
	{ "peer",	Lpeer},
	{ "cipher",	Lcipher_info},
	{ "name",	Lname},
	{ "settimeout",	Lsettimeout},
	{ "keycert",Lkeycert},
	{ "connect",	Lconnect	},
	{ NULL,		NULL	}
};

#ifdef EXPORT_HASH_FUNCTIONS
static const luaL_reg Rhash[] = 
{
	{ "update",	Lhash_update},
	{ "digest",	Lhash_digest},
	{ "reset",	Lhash_reset},
	{ NULL,		NULL	}
};
#endif

static const luaL_reg Raes[] = 
{
    { "encrypt", Laes_encrypt},
    { "decrypt", Laes_decrypt},
    { "cbc_encrypt", Laes_cbc_encrypt},
    { "cbc_decrypt", Laes_cbc_decrypt},
    { "cfb_encrypt", Laes_cfb_encrypt},
    { "cfb_decrypt", Laes_cfb_decrypt},
	{ NULL,		NULL	}
};

static const luaL_reg Rrc4[] = 
{
    { "crypt", Lrc4_crypt},
	{ NULL,		NULL	}
};

static const luaL_reg Rm[] = {
	{ "ssl",	Lssl	},
	{ "rand",	Lrand	},
	{ "aes",	Laes	},
	{ "rc4",	Lrc4	},
#ifdef EXPORT_HASH_FUNCTIONS
	{ "hash",	Lhash	},
#endif
	{ NULL,		NULL	}
};

LUA_API int luaopen_lxyssl(lua_State *L)
{
 havege_init( &hs );

 luaL_newmetatable(L,MYTYPE);
 lua_pushliteral(L,"__index");
 lua_pushvalue(L,-2);
 lua_settable(L,-3);
 luaL_openlib(L,NULL,R,0);

#ifdef EXPORT_HASH_FUNCTIONS
 luaL_newmetatable(L,MYHASH);
 lua_pushliteral(L,"__index");
 lua_pushvalue(L,-2);
 lua_settable(L,-3);
 luaL_openlib(L,NULL,Rhash,0);
#endif

 luaL_newmetatable(L,MYAES);
 lua_pushliteral(L,"__index");
 lua_pushvalue(L,-2);
 lua_settable(L,-3);
 luaL_openlib(L,NULL,Raes,0);

 luaL_newmetatable(L,MYRC4);
 lua_pushliteral(L,"__index");
 lua_pushvalue(L,-2);
 lua_settable(L,-3);
 luaL_openlib(L,NULL,Rrc4,0);
 #if 0
 lua_pushliteral (L, "__metatable");
 lua_pushliteral (L, MYTYPE" you're not allowed to get this metatable");
 lua_settable (L, -3);
 #endif

 luaL_openlib(L,"lxyssl",Rm,0);
 lua_pushliteral(L,"version");			/** version */
 lua_pushliteral(L,MYVERSION);
 lua_settable(L,-3);

 return 1;
}
