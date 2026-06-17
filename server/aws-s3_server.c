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

#define MAX_KEY_LEN   256
#define MAX_ENTRIES   10000   // capacidad inicial del bucket
#define MAX_FREE      10000

typedef struct {
    char     magic[4];     // "BKT1" para identificar el formato
    uint32_t max_entries;
    uint32_t num_entries;  // entradas de directorio ocupadas
    uint32_t max_free;
    uint32_t num_free;     // entradas de free-list ocupadas
    uint64_t data_start;   // offset donde empieza la zona de datos
} BucketHeader;

typedef struct {
    char     key[MAX_KEY_LEN];  // clave/prefijo completo, "" si está libre
    uint64_t offset;            // byte donde empieza el contenido
    uint64_t size;               // tamaño en bytes del objeto
    int      used;               // 1 = slot ocupado, 0 = libre
} DirEntry;

typedef struct {
    uint64_t offset;
    uint64_t size;
    int      used;   // 1 = hueco libre válido, 0 = slot de la lista sin usar
} FreeBlock;

#define BUCKETS_DIR "./buckets"

static void bucket_path(const char *bucket_name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s.bkt", BUCKETS_DIR, bucket_name);
}

static uint64_t compute_data_start(void) {
    return sizeof(BucketHeader)
         + (uint64_t)MAX_ENTRIES * sizeof(DirEntry)
         + (uint64_t)MAX_FREE * sizeof(FreeBlock);
}

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

int bucket_read_header(FILE *f, BucketHeader *header) {
    fseek(f, 0, SEEK_SET);
    if (fread(header, sizeof(BucketHeader), 1, f) != 1) return -1;
    if (memcmp(header->magic, "BKT1", 4) != 0) {
        fprintf(stderr, "Error: formato de bucket inválido\n");
        return -1;
    }
    return 0;
}

int bucket_write_header(FILE *f, BucketHeader *header) {
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

int bucket_read_entry(FILE *f, int i, DirEntry *e) {
    fseek(f, dir_entry_offset(i), SEEK_SET);
    return fread(e, sizeof(DirEntry), 1, f) == 1 ? 0 : -1;
}
int bucket_write_entry(FILE *f, int i, DirEntry *e) {
    fseek(f, dir_entry_offset(i), SEEK_SET);
    return fwrite(e, sizeof(DirEntry), 1, f) == 1 ? 0 : -1;
}
int bucket_read_free(FILE *f, int i, FreeBlock *b) {
    fseek(f, free_block_offset(i), SEEK_SET);
    return fread(b, sizeof(FreeBlock), 1, f) == 1 ? 0 : -1;
}
int bucket_write_free(FILE *f, int i, FreeBlock *b) {
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

int bucket_put_object(const char *bucket_name, const char *key,
                       const unsigned char *data, uint64_t size) {
    FILE *f = bucket_open(bucket_name, "r+b");
    if (!f) return -1;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return -1; }

    int idx = find_entry_by_key(f, &header, key);

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

    int new_idx = find_free_dir_slot(f, &header);
    if (new_idx < 0) {
        fprintf(stderr, "Error: directorio del bucket lleno\n");
        fclose(f); return -1;
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

unsigned char *bucket_get_object(const char *bucket_name, const char *key, uint64_t *out_size) {
    FILE *f = bucket_open(bucket_name, "rb");
    if (!f) return NULL;

    BucketHeader header;
    if (bucket_read_header(f, &header) != 0) { fclose(f); return NULL; }

    int idx = find_entry_by_key(f, &header, key);
    if (idx < 0) {
        fprintf(stderr, "Error: clave '%s' no encontrada\n", key);
        fclose(f); return NULL;
    }

    DirEntry e; bucket_read_entry(f, idx, &e);
    unsigned char *buffer = malloc(e.size);
    if (!buffer) { fclose(f); return NULL; }

    fseek(f, e.offset, SEEK_SET);
    fread(buffer, 1, e.size, f);
    fclose(f);

    *out_size = e.size;
    return buffer; // el llamador debe hacer free()
}

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

void bucket_list_buckets(void) {
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
        return -1;
    }

    char path[512];
    bucket_path(bucket_name, path, sizeof(path));
    if (remove(path) != 0) { perror("remove"); return -1; }

    printf("Bucket '%s' eliminado\n", bucket_name);
    return 0;
}

int main(void) {
    //main de prueba, mientras se implementan los sockets y el cliente
    bucket_create("mi-bucket");

    const char *contenido = "Hola mundo";
    bucket_put_object("mi-bucket", "saludo.txt",
                       (const unsigned char *)contenido, strlen(contenido));

    bucket_list_objects("mi-bucket", "");

    uint64_t size;
    unsigned char *data = bucket_get_object("mi-bucket", "saludo.txt", &size);
    if (data) {
        printf("Contenido recuperado (%llu bytes): %.*s\n",
               (unsigned long long)size, (int)size, data);
        free(data);
    }

    bucket_delete_object("mi-bucket", "saludo.txt");
    bucket_remove("mi-bucket", 0);

    return 0;
}