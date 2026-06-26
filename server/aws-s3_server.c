/* @authors: Fernando Sánchez Hidalgo - 2022218688
             Kevin Espinoza Barrantes - 2023055841
    Principios de sistemas operativos: Proyecto 3
    @see estándar de formato: https://users.ece.cmu.edu/~eno/coding/CCodingStandard.html#cdef
    @see compilado con GCC: https://gcc.gnu.org
Compilar: gcc -o aws-s3_server aws-s3_server.c
Ejecutar: ./aws-s3_server
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* ============================================================
 *  Constantes
 * ============================================================ */
#define MAX_KEY_LEN   256
#define MAX_ENTRIES   10000   // capacidad inicial del bucket
#define MAX_FREE      10000
#define BUCKETS_DIR   "./buckets"
#define SERVER_PORT   8080
#define BACKLOG       10

/* ============================================================
 *  Protocolo de mensajes (cliente ↔ servidor)
 *
 *  Cada mensaje comienza con un MsgHeader fijo.
 *  Los campos de longitud permiten transmitir datos binarios.
 *
 *  Tipos de comando (cmd):
 *    CMD_LS      = 1   aws-s3 ls [s3://bucket/prefix]
 *    CMD_MB      = 2   aws-s3 mb s3://bucket
 *    CMD_PUT     = 3   aws-s3 cp local → s3  (datos adjuntos)
 *    CMD_GET     = 4   aws-s3 cp s3 → local
 *    CMD_MV      = 5   aws-s3 mv src dst
 *    CMD_RM      = 6   aws-s3 rm s3://bucket/key [--recursive]
 *    CMD_SYNC    = 7   aws-s3 sync (múltiples PUT enviados por el cliente)
 *    CMD_RB      = 8   aws-s3 rb s3://bucket [--force]
 *
 *  Tipos de respuesta (cmd en la respuesta):
 *    RESP_OK     = 100
 *    RESP_ERROR  = 101
 *    RESP_DATA   = 102  (respuesta con datos adjuntos, p.ej. GET)
 *    RESP_LIST   = 103  (respuesta con texto de listado)
 * ============================================================ */
#define CMD_LS    1
#define CMD_MB    2
#define CMD_PUT   3
#define CMD_GET   4
#define CMD_MV    5
#define CMD_RM    6
#define CMD_SYNC  7
#define CMD_RB    8

#define RESP_OK    100
#define RESP_ERROR 101
#define RESP_DATA  102
#define RESP_LIST  103

#define FLAG_RECURSIVE 0x01
#define FLAG_FORCE     0x02
#define FLAG_DELETE    0x04

/* Cabecera fija (20 bytes) que precede a toda transmisión */
typedef struct __attribute__((packed)) {
    uint8_t  cmd;          /* tipo de comando / respuesta  */
    uint8_t  flags;        /* bits auxiliares (--recursive=1, --force=2, --delete=4) */
    uint16_t bucket_len;   /* longitud del nombre del bucket (sin '\0') */
    uint16_t key_len;      /* longitud de la clave/prefijo   (sin '\0') */
    uint16_t key2_len;     /* clave destino en mv            (sin '\0') */
    uint64_t data_len;     /* longitud de los datos adjuntos */
} MsgHeader;

/* ============================================================
 *  Estructuras del bucket
 * ============================================================ */
typedef struct {
    char     magic[4];     // firma "BKT1" para archivos binarios
    uint32_t max_entries;
    uint32_t num_entries;  // entradas de directorio ocupadas
    uint32_t max_free;
    uint32_t num_free;     // entradas de free-list ocupadas
    uint64_t data_start;   // offset donde empieza la zona de datos
} BucketHeader;

typedef struct {
    char     key[MAX_KEY_LEN];      // clave/prefijo completo, "" si está libre
    uint64_t offset;                // byte donde empieza el contenido
    uint64_t size;                  // tamaño en bytes del objeto
    int      used;                  // 1 = slot ocupado, 0 = libre
} DirEntry;

typedef struct {
    uint64_t offset;
    uint64_t size;
    int      used;   // 1 = hueco libre válido, 0 = slot de la lista sin usar
} FreeBlock;

/* ============================================================
 *  Helpers de E/S segura sobre sockets
 * ============================================================ */

/*
 * net_send_all – Envía exactamente `len` bytes al socket.
 * @param fd    file descriptor del cleinte
 * @param buf   contenido a enviar
 * @param len   tamaño del contenido
 * @return      0 sin errores
 *              -1 en error / conexión cerrada.
 */
static int net_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p   += n;
        len -= n;
    }
    return 0;
}

/*
 * net_recv_all – Recibe exactamente `len` bytes del socket.
 * @param fd    file descriptor del cleinte
 * @param buf   contenido a recibir
 * @param len   tamaño del contenido
 * @return      0 sin errores
 *              -1 en error / conexión cerrada.
 */
static int net_recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return -1;
        p   += n;
        len -= n;
    }
    return 0;
}

/*
 * send_response – Envía una respuesta simple (sin datos adjuntos).
 * @param fd      socket del cliente
 * @param type    RESP_OK | RESP_ERROR
 * @param msg     mensaje de texto (puede ser NULL)
 */
static void send_response(int fd, uint8_t type, const char *msg) {
    MsgHeader h;
    memset(&h, 0, sizeof(h));
    h.cmd      = type;
    h.data_len = msg ? (uint64_t)strlen(msg) : 0;
    net_send_all(fd, &h, sizeof(h));
    if (msg && h.data_len > 0)
        net_send_all(fd, msg, (size_t)h.data_len);
}

/*
 * send_data_response – Envía una respuesta con datos binarios adjuntos.
 * @param fd        file descriptor del client
 * @param type      RESP_DATA | RESP_LIST
 * @param data      contenido a enviar
 * @param data_len  tamaño del contenido
 */
static void send_data_response(int fd, uint8_t type, const void *data, uint64_t data_len) {
    MsgHeader h;
    memset(&h, 0, sizeof(h));
    h.cmd      = type;
    h.data_len = data_len;
    net_send_all(fd, &h, sizeof(h));
    if (data && data_len > 0)
        net_send_all(fd, data, (size_t)data_len);
}

/* ============================================================
 *  Funciones de gestión de buckets
 * ============================================================ */

static void bucket_path(const char *bucket_name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s.bkt", BUCKETS_DIR, bucket_name);
}

static uint64_t compute_data_start(void) {
    return sizeof(BucketHeader)
         + (uint64_t)MAX_ENTRIES * sizeof(DirEntry)
         + (uint64_t)MAX_FREE    * sizeof(FreeBlock);
}

/*
    * bucket_create – Crea un bucket nuevo, genera el header y asigna memoria
    *
    * @param bucket_name   nombre del bucket
    * @return 0: Sin errores
    */
int bucket_create(const char *bucket_name) {
    mkdir(BUCKETS_DIR, 0755); // si ya existe, mkdir falla pero no es un error fatal

    char path[512];
    bucket_path(bucket_name, path, sizeof(path));

    if (access(path, F_OK) == 0) {
        fprintf(stderr, "Error: el bucket '%s' ya existe\n", bucket_name);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return -1; }

    BucketHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "BKT1", 4);
    header.max_entries = MAX_ENTRIES;
    header.max_free = MAX_FREE;
    header.data_start = compute_data_start();
    fwrite(&header, sizeof(header), 1, f);

    DirEntry empty_entry; memset(&empty_entry, 0, sizeof(empty_entry));
    for (uint32_t i = 0; i < MAX_ENTRIES; i++) fwrite(&empty_entry, sizeof(empty_entry), 1, f);

    FreeBlock empty_free; memset(&empty_free, 0, sizeof(empty_free));
    for (uint32_t i = 0; i < MAX_FREE; i++) fwrite(&empty_free, sizeof(empty_free), 1, f);

    fclose(f);
    printf("Bucket '%s' creado correctamente\n", bucket_name);
    return 0;
}

FILE *bucket_open(const char *bucket_name, const char *mode) {
    char path[512];
    bucket_path(bucket_name, path, sizeof(path));
    FILE *f = fopen(path, mode);
    if (!f) perror("fopen");
    return f;
}

static int bucket_read_header(FILE *f, BucketHeader *header) {
    fseek(f, 0, SEEK_SET);
    if (fread(header, sizeof(BucketHeader), 1, f) != 1) return -1;
    if (memcmp(header->magic, "BKT1", 4) != 0) {
        fprintf(stderr, "Error: formato de bucket inválido\n");
        return -1;
    }
    return 0;
}

static int bucket_write_header(FILE *f, BucketHeader *header) {
    fseek(f, 0, SEEK_SET);
    return fwrite(header, sizeof(BucketHeader), 1, f) == 1 ? 0 : -1;
}

static long dir_entry_offset(int i) {
    return (long)sizeof(BucketHeader) + (long)i * sizeof(DirEntry);
}
static long free_block_offset(int i) {
    return (long)sizeof(BucketHeader) + (long)MAX_ENTRIES * sizeof(DirEntry)
         + (long)i * sizeof(FreeBlock);
}

static int bucket_read_entry(FILE *f, int i, DirEntry *e) {
    fseek(f, dir_entry_offset(i), SEEK_SET);
    return fread(e, sizeof(DirEntry), 1, f) == 1 ? 0 : -1;
}
static int bucket_write_entry(FILE *f, int i, DirEntry *e) {
    fseek(f, dir_entry_offset(i), SEEK_SET);
    return fwrite(e, sizeof(DirEntry), 1, f) == 1 ? 0 : -1;
}
static int bucket_read_free(FILE *f, int i, FreeBlock *b) {
    fseek(f, free_block_offset(i), SEEK_SET);
    return fread(b, sizeof(FreeBlock), 1, f) == 1 ? 0 : -1;
}
static int bucket_write_free(FILE *f, int i, FreeBlock *b) {
    fseek(f, free_block_offset(i), SEEK_SET);
    return fwrite(b, sizeof(FreeBlock), 1, f) == 1 ? 0 : -1;
}

static int find_entry_by_key(FILE *f, BucketHeader *h, const char *key) {
    for (uint32_t i = 0; i < h->max_entries; i++) {
        DirEntry e; bucket_read_entry(f, i, &e);
        if (e.used && strcmp(e.key, key) == 0) return (int)i;
    }
    return -1;
}

static int find_free_dir_slot(FILE *f, BucketHeader *h) {
    for (uint32_t i = 0; i < h->max_entries; i++) {
        DirEntry e; bucket_read_entry(f, i, &e);
        if (!e.used) return (int)i;
    }
    return -1;
}

static int add_free_block(FILE *f, BucketHeader *h, uint64_t offset, uint64_t size) {
    for (uint32_t i = 0; i < h->max_free; i++) {
        FreeBlock b; bucket_read_free(f, i, &b);
        if (!b.used) {
            b.offset = offset; b.size = size; b.used = 1;
            bucket_write_free(f, i, &b);
            return 0;
        }
    }
    return -1; // free list llena, caso límite a documentar
}

static uint64_t data_end_offset(FILE *f, BucketHeader *h) {
    uint64_t max_end = h->data_start;
    for (uint32_t i = 0; i < h->max_entries; i++) {
        DirEntry e; bucket_read_entry(f, i, &e);
        if (e.used) {
            uint64_t end = e.offset + e.size;
            if (end > max_end) max_end = end;
        }
    }
    return max_end;
}

// first-fit: primer hueco libre que ajuste, o el final del archivo
static uint64_t allocate_space(FILE *f, BucketHeader *h, uint64_t size, int *free_idx) {
    *free_idx = -1;
    for (uint32_t i = 0; i < h->max_free; i++) {
        FreeBlock b; bucket_read_free(f, i, &b);
        if (b.used && b.size >= size) { *free_idx = (int)i; return b.offset; }
    }
    return data_end_offset(f, h);
}

    /*
    * bucket_put_objetct – insertar un objeto en el bucket
    *
    * @param bucket_name    nombre del bucket
    * @param key            identificador del object
    * @param data           datos del objeto
    * @param size           tamaño del objeto
    * @return 0: Sin errores
    */
static int bucket_put_object(const char *bucket_name, const char *key, const unsigned char *data, uint64_t size) {
    char path[512]; bucket_path(bucket_name, path, sizeof(path));
    if (access(path, F_OK) != 0) return -1; /* bucket no existe */
    FILE *f = bucket_open(bucket_name, "r+b");
    if (!f) return -1;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return -1; }

    int idx = find_entry_by_key(f, &header, key);

    //si el bucket ya tiene objetos, agregar nuevo id al final
    if (idx >= 0) {
        DirEntry old; bucket_read_entry(f, idx, &old);

        if (old.size == size) {
            fseek(f, old.offset, SEEK_SET);
            fwrite(data, 1, size, f);
            fclose(f);
            return 0;
        }

        int free_idx;
        uint64_t new_offset = allocate_space(f, &header, size, &free_idx);
        fseek(f, new_offset, SEEK_SET);
        fwrite(data, 1, size, f);

        if (free_idx >= 0) {
            FreeBlock b; bucket_read_free(f, free_idx, &b);
            b.used = 0; bucket_write_free(f, free_idx, &b);
        }

        add_free_block(f, &header, old.offset, old.size);

        old.offset = new_offset; old.size = size;
        bucket_write_entry(f, idx, &old);
        bucket_write_header(f, &header);
        fclose(f);
        return 0;
    }

    //si el bucket no tiene espacios libres ==> error
    int new_idx = find_free_dir_slot(f, &header);
    if (new_idx < 0) {
        fprintf(stderr, "Error: directorio del bucket lleno\n");
        fclose(f); 
        return -1;
    }

    int free_idx;
    uint64_t offset = allocate_space(f, &header, size, &free_idx);
    fseek(f, offset, SEEK_SET);
    fwrite(data, 1, size, f);

    if (free_idx >= 0) {
        FreeBlock b; bucket_read_free(f, free_idx, &b);
        b.used = 0; bucket_write_free(f, free_idx, &b);
    }

    DirEntry e; memset(&e, 0, sizeof(e));
    strncpy(e.key, key, MAX_KEY_LEN - 1);
    e.offset = offset; e.size = size; e.used = 1;
    bucket_write_entry(f, new_idx, &e);

    bucket_write_header(f, &header);
    fclose(f);
    return 0;
}

/*
    * bucket_get_object – Busca y retorna el contenido del object
    *
    * @param bucket_name    nombre del bucket donde está el object
    * @param key            identificador del object
    * @param out_size       (salida) tamaño del object
    * @return               data del objeto
    */
static unsigned char *bucket_get_object(const char *bucket_name, const char *key, uint64_t *out_size) {
    
    FILE *f = bucket_open(bucket_name, "rb");
    if (!f) return NULL;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return NULL; }

    int idx = find_entry_by_key(f, &header, key);
    if (idx < 0) {
        fprintf(stderr, "Error: clave '%s' no encontrada\n", key);
        fclose(f); 
        return NULL;
    }

    DirEntry e; bucket_read_entry(f, idx, &e);
    unsigned char *buffer = malloc(e.size);
    if (!buffer) { 
        fclose(f); 
        return NULL; 
    }

    fseek(f, e.offset, SEEK_SET);
    fread(buffer, 1, e.size, f);
    fclose(f);

    *out_size = e.size;
    return buffer; // el llamador debe hacer free()
}

/*
* bucket_delete_object – Elimina un object del bucket
*
* @param bucket_name    nombre del bucket
* @param key            identificador del object
* @return               0: Sin errores
*/
int bucket_delete_object(const char *bucket_name, const char *key) {
    FILE *f = bucket_open(bucket_name, "r+b");
    if (!f) return -1;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return -1; }

    int idx = find_entry_by_key(f, &header, key);
    if (idx < 0) {
        fprintf(stderr, "Error: clave '%s' no encontrada\n", key);
        fclose(f); return -1;
    }

    DirEntry e; bucket_read_entry(f, idx, &e);
    add_free_block(f, &header, e.offset, e.size);

    e.used = 0;
    memset(e.key, 0, MAX_KEY_LEN);
    bucket_write_entry(f, idx, &e);
    bucket_write_header(f, &header);

    fclose(f);
    return 0;
}

/*
* list_buckets – Lista todos los buckets
*
* @param bucket_name    nombre del bucket
* @param key            identificador del object
*/
void list_buckets(void) {

    DIR *d = opendir(BUCKETS_DIR);
    if (!d) { perror("opendir"); return; }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".bkt") == 0) {
            printf("%.*s\n", (int)(len - 4), entry->d_name);
        }
    }
    closedir(d);
}

/*
* bucket_list_objects – Lista de todos los objetos del bucket
*
* @param bucket_name   nombre del bucket
* @param prefix        (opcional) filtra todos los objects que empiecen con prefix
*/
void bucket_list_objects(const char *bucket_name, const char *prefix) {
    
    FILE *f = bucket_open(bucket_name, "rb");
    if (!f) return;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return; }

    size_t plen = prefix ? strlen(prefix) : 0;
    for (uint32_t i = 0; i < header.max_entries; i++) {
        DirEntry e; bucket_read_entry(f, i, &e);
        if (!e.used) continue;
        if (plen == 0 || strncmp(e.key, prefix, plen) == 0) {
            printf("%10llu  %s\n", (unsigned long long)e.size, e.key);
        }
    }
    fclose(f);
}

int bucket_remove(const char *bucket_name, int force) {
        /*
    * bucket_delete_object – Elimina un bucket
    *
    * @param bucket_name    nombre del bucket
    * @param force          (opcional) 1: forzar borrado en caso que el bucket no esté vacío
    * @return               0: Sin errores
    *                       -1: salida con errores
    *                       -2: el bucket tiene objetos (sin force)
    */
    FILE *f = bucket_open(bucket_name, "rb");
    if (!f) return -1;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return -1; }

    int has_objects = 0;
    for (uint32_t i = 0; i < header.max_entries && !has_objects; i++) {
        DirEntry e; bucket_read_entry(f, i, &e);
        if (e.used) has_objects = 1;
    }
    fclose(f);

    if (has_objects && !force) {
        fprintf(stderr, "Error: el bucket '%s' no está vacío (use --force)\n", bucket_name);
        return -2;
    }

    char path[512];
    bucket_path(bucket_name, path, sizeof(path));
    if (remove(path) != 0) { perror("remove"); return -1; }

    printf("Bucket '%s' eliminado\n", bucket_name);
    return 0;
}

/* ============================================================
 *  Manejadores de comandos (uno por CMD_*)
 * ============================================================ */

/*
 * handle_ls – Lista buckets o el contenido de un bucket/prefijo.
 * Si bucket == "" se listan todos los buckets.
 * @param fd            file descriptor del cleinte
 * @param bucket_name   nombre del bucket
 * @param prefix        (opcional) filtro
 */
static void handle_ls(int fd, const char *bucket_name, const char *prefix) {
    char out[65536]; /* buffer de texto de respuesta */
    int  pos = 0;

    if (bucket_name[0] == '\0') {
        /* listar todos los buckets */
        DIR *d = opendir(BUCKETS_DIR);
        if (!d) { send_response(fd, RESP_ERROR, "No se puede abrir el directorio de buckets"); return; }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len > 4 && strcmp(de->d_name + len - 4, ".bkt") == 0) {
                pos += snprintf(out + pos, sizeof(out) - pos,
                                "%.*s\n", (int)(len - 4), de->d_name);
            }
        }
        closedir(d);
    } else {
        /* listar objetos dentro del bucket */
        char path[512]; bucket_path(bucket_name, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) { send_response(fd, RESP_ERROR, "Bucket no encontrado"); return; }

        BucketHeader h; if (bucket_read_header(f, &h) != 0) { fclose(f); send_response(fd, RESP_ERROR, "Formato de bucket inválido"); return; }

        size_t plen = prefix ? strlen(prefix) : 0;
        for (uint32_t i = 0; i < h.max_entries; i++) {
            DirEntry e; bucket_read_entry(f, i, &e);
            if (!e.used) continue;
            if (plen == 0 || strncmp(e.key, prefix, plen) == 0)
                pos += snprintf(out + pos, sizeof(out) - pos,
                                "%10llu  %s\n",
                                (unsigned long long)e.size, e.key);
        }
        fclose(f);
    }
    out[pos] = '\0';
    send_data_response(fd, RESP_LIST, out, (uint64_t)pos);
}

/*
 * handle_mb – Crea un nuevo bucket.
 * @param fd            file descriptor del cleinte
 * @param bucket_name   nombre del bucket
 */
static void handle_mb(int fd, const char *bucket_name) {
    if (bucket_create(bucket_name) == 0)
        send_response(fd, RESP_OK, "Bucket creado correctamente");
    else
        send_response(fd, RESP_ERROR, "Error al crear el bucket (ya existe o error de I/O)");
}

/*
 * handle_put – Almacena un objeto en el bucket.
 * Los datos binarios llegan como parte del mensaje.
 * @param fd            file descriptor del cliente
 * @param bucket_name   nombre del bucket
 * @param key           identificador del object
 * @param data          contenido del object
 * @param data_len      tamaño del contenido
 */
static void handle_put(int fd, const char *bucket_name, const char *key, const unsigned char *data, uint64_t data_len) {
    /* Si el bucket no existe lo creamos automáticamente */
    char path[512]; bucket_path(bucket_name, path, sizeof(path));
    if (access(path, F_OK) != 0) bucket_create(bucket_name);

    if (bucket_put_object(bucket_name, key, data, data_len) == 0)
        send_response(fd, RESP_OK, "Objeto almacenado correctamente");
    else
        send_response(fd, RESP_ERROR, "Error al almacenar el objeto");
}

/*
 * handle_get – Recupera un objeto del bucket y lo envía al cliente.
 * @param bucket    nombre del bucket
 * @param key       identificador del objeto
 */
static void handle_get(int fd, const char *bucket_name, const char *key) {
    uint64_t size;
    unsigned char *data = bucket_get_object(bucket_name, key, &size);
    if (!data) {
        send_response(fd, RESP_ERROR, "Objeto no encontrado");
        return;
    }
    send_data_response(fd, RESP_DATA, data, size);
    free(data);
}

/*
 * handle_mv – Mueve (copia + elimina origen) un objeto.
 * @param bucket_src:   ruta del bucket origen
 * @param key_src:      identificador del object origen
 * @param bucket_dst    ruta del bucket destino
 * @param key_dst       identificador del object destino
 */
static void handle_mv(int fd, const char *bucket_src, const char *key_src, const char *bucket_dst, const char *key_dst) {
    uint64_t size;
    unsigned char *data = bucket_get_object(bucket_src, key_src, &size);
    if (!data) { send_response(fd, RESP_ERROR, "Objeto origen no encontrado"); return; }

    char path[512]; bucket_path(bucket_dst, path, sizeof(path));
    if (access(path, F_OK) != 0) bucket_create(bucket_dst);

    if (bucket_put_object(bucket_dst, key_dst, data, size) != 0) {
        free(data);
        send_response(fd, RESP_ERROR, "Error al escribir en el destino");
        return;
    }
    free(data);
    bucket_delete_object(bucket_src, key_src);
    send_response(fd, RESP_OK, "Objeto movido correctamente");
}

/*
 * handle_rm – Elimina un objeto
 * @param bucket_name   nombre del bucket
 * @param key           identificador del object
 *                      prefijo si --recursive
 * @param recursive     0: solo elimina el object
 *                      1: elimina los objects con prefijo <key> (todos si key es vacío)
 */
static void handle_rm(int fd, const char *bucket_name, const char *key, int recursive) {
    if (!recursive) {
        if (bucket_delete_object(bucket_name, key) == 0)
            send_response(fd, RESP_OK, "Objeto eliminado");
        else
            send_response(fd, RESP_ERROR, "Objeto no encontrado");
        return;
    }

    /* modo --recursive: borrar todo lo que empiece con el prefijo */
    char path[512]; bucket_path(bucket_name, path, sizeof(path));
    FILE *f = fopen(path, "r+b");
    if (!f) { send_response(fd, RESP_ERROR, "Bucket no encontrado"); return; }

    BucketHeader h; if (bucket_read_header(f, &h) != 0) { fclose(f); send_response(fd, RESP_ERROR, "Formato inválido"); return; }
    fclose(f);

    size_t plen = strlen(key);
    int deleted = 0;
    for (uint32_t i = 0; i < h.max_entries; i++) {
        /* re-abrir en modo lectura para obtener la clave */
        FILE *fr = fopen(path, "rb");
        if (!fr) break;
        BucketHeader htmp; bucket_read_header(fr, &htmp);
        DirEntry e; bucket_read_entry(fr, i, &e);
        fclose(fr);
        if (!e.used) continue;
        if (plen == 0 || strncmp(e.key, key, plen) == 0) {
            bucket_delete_object(bucket_name, e.key);
            deleted++;
        }
    }
    char msg[64]; snprintf(msg, sizeof(msg), "%d objeto(s) eliminado(s)", deleted);
    send_response(fd, RESP_OK, msg);
}

/*
 * handle_rb – Elimina un bucket.
 * @param fd        file descriptor del cliente
 * @param bucket    nombre del bucket
 * @param force     1: forzar borrado
 */
static void handle_rb(int fd, const char *bucket_name, int force) {
    int r = bucket_remove(bucket_name, force);
    if (r == 0)       send_response(fd, RESP_OK,    "Bucket eliminado");
    else if (r == -2) send_response(fd, RESP_ERROR, "El bucket no está vacío (use --force)");
    else              send_response(fd, RESP_ERROR, "Error al eliminar el bucket");
}

/* ============================================================
 *  Bucle de atención a un cliente
 * ============================================================ */

/*
 * recv_string – Lee una cadena de `len` bytes del socket y la almacena
 *               en `buf` (agrega '\0' al final). `buf_size` debe ser > len.
 * @param fd        file descriptor del cliente
 * @param buf       contenido en el socket
 * @param len       tamaño del contenido
 * @param buf_size  tamaño límite
 * @return          0 sin error
 *                  -1 salida con errores
 */
static int recv_string(int fd, char *buf, uint16_t len, size_t buf_size) {
    if (len == 0) { buf[0] = '\0'; return 0; }
    if (len >= buf_size) return -1;
    if (net_recv_all(fd, buf, len) != 0) return -1;
    buf[len] = '\0';
    return 0;
}

/*
 * handle_client – Atiende los mensajes de un cliente hasta que cierre
 *                 la conexión.
 * @param client_fd:    file descriptor para comunicación con el client
 */
static void handle_client(int client_fd) {
    printf("[servidor] cliente conectado (fd=%d)\n", client_fd);

    while (1) {
        MsgHeader hdr;
        int r = net_recv_all(client_fd, &hdr, sizeof(hdr));
        if (r != 0) break; /* cliente desconectado o error */

        /* leer bucket, key y key2 */
        char bucket[MAX_KEY_LEN + 1] = {0};
        char key[MAX_KEY_LEN    + 1] = {0};
        char key2[MAX_KEY_LEN   + 1] = {0};

        if (recv_string(client_fd, bucket, hdr.bucket_len, sizeof(bucket)) != 0) break;
        if (recv_string(client_fd, key,    hdr.key_len,    sizeof(key))    != 0) break;
        if (recv_string(client_fd, key2,   hdr.key2_len,   sizeof(key2))   != 0) break;

        /* leer datos adjuntos si los hay */
        unsigned char *data = NULL;
        if (hdr.data_len > 0) {
            data = malloc((size_t)hdr.data_len);
            if (!data || net_recv_all(client_fd, data, (size_t)hdr.data_len) != 0) {
                free(data);
                break;
            }
        }

        int flags     = hdr.flags;
        int recursive = (flags & FLAG_RECURSIVE) != 0;
        int force     = (flags & FLAG_FORCE)     != 0;

        switch (hdr.cmd) {
            case CMD_LS:
                printf("[cmd] LS bucket='%s' prefix='%s'\n", bucket, key);
                handle_ls(client_fd, bucket, key);
                break;

            case CMD_MB:
                printf("[cmd] MB bucket='%s'\n", bucket);
                handle_mb(client_fd, bucket);
                break;

            case CMD_PUT:
                printf("[cmd] PUT bucket='%s' key='%s' size=%llu\n",
                       bucket, key, (unsigned long long)hdr.data_len);
                handle_put(client_fd, bucket, key, data, hdr.data_len);
                break;

            case CMD_GET:
                printf("[cmd] GET bucket='%s' key='%s'\n", bucket, key);
                handle_get(client_fd, bucket, key);
                break;

            case CMD_MV: {
                /*
                 * key2 contiene el bucket destino + clave destino
                 * en el formato "bucket_dst\nkey_dst"
                 */
                char bucket_dst[MAX_KEY_LEN + 1] = {0};
                char key_dst   [MAX_KEY_LEN + 1] = {0};
                char *sep = strchr(key2, '\n');
                if (sep) {
                    size_t blen = (size_t)(sep - key2);
                    if (blen >= sizeof(bucket_dst)) blen = sizeof(bucket_dst) - 1;
                    memcpy(bucket_dst, key2, blen);
                    bucket_dst[blen] = '\0';
                    strncpy(key_dst, sep + 1, sizeof(key_dst) - 1);
                } else {
                    /* mismo bucket */
                    strncpy(bucket_dst, bucket, sizeof(bucket_dst) - 1);
                    strncpy(key_dst,    key2,   sizeof(key_dst)    - 1);
                }
                printf("[cmd] MV %s/%s → %s/%s\n", bucket, key, bucket_dst, key_dst);
                handle_mv(client_fd, bucket, key, bucket_dst, key_dst);
                break;
            }

            case CMD_RM:
                printf("[cmd] RM bucket='%s' key='%s' recursive=%d\n",
                       bucket, key, recursive);
                handle_rm(client_fd, bucket, key, recursive);
                break;

            case CMD_SYNC:
                /*
                 * SYNC funciona como PUT repetido; el cliente envía
                 * los archivos uno a uno con CMD_PUT. Este comando
                 * sólo confirma que el servidor está listo.
                 */
                printf("[cmd] SYNC bucket='%s' prefix='%s'\n", bucket, key);
                send_response(client_fd, RESP_OK, "SYNC: envíe los archivos con CMD_PUT");
                break;

            case CMD_RB:
                printf("[cmd] RB bucket='%s' force=%d\n", bucket, force);
                handle_rb(client_fd, bucket, force);
                break;

            default:
                fprintf(stderr, "[servidor] comando desconocido: %d\n", hdr.cmd);
                send_response(client_fd, RESP_ERROR, "Comando desconocido");
                break;
        }

        free(data);
        data = NULL;
    }

    printf("[servidor] cliente desconectado (fd=%d)\n", client_fd);
    close(client_fd);
}

/* ============================================================
 *  main – arranca el servidor TCP
 * ============================================================ */
int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    /* reutilizar el puerto inmediatamente tras reiniciar el servidor */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }

    printf("[servidor] escuchando en puerto %d...\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        printf("[servidor] conexión desde %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}