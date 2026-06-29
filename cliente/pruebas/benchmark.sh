#!/bin/bash
# =============================================================
#  benchmark.sh – Pruebas de rendimiento para aws-s3
#  Nota: el servidor debe estar corriendo al momento de ejecutar las pruebas
#  Uso: ./benchmark.sh
# =============================================================

AWS="./aws-s3"           # ruta al cliente
TMPDIR="/tmp/aws_bench"  # directorio temporal local
RESULTS="resultados_benchmark.txt"

mkdir -p "$TMPDIR"
> "$RESULTS"

log() { echo "$1" | tee -a "$RESULTS"; }

# -------------------------------------------------------------
#  Función auxiliar: mide tiempo de un comando y lo registra
#  Uso: medir "descripcion" comando arg1 arg2 ...
# -------------------------------------------------------------
medir() {
    local desc="$1"; shift
    local start end elapsed
    start=$(date +%s%3N)          # milisegundos
    "$@" > /dev/null 2>&1
    end=$(date +%s%3N)
    elapsed=$((end - start))
    log "  $desc: ${elapsed} ms"
}

# -------------------------------------------------------------
#  Generar archivos de prueba de distintos tamaños
# -------------------------------------------------------------
generar_archivos() {
    log "Generando archivos de prueba..."
    dd if=/dev/urandom of="$TMPDIR/small.bin"  bs=1K   count=10   2>/dev/null   # 10 KB
    dd if=/dev/urandom of="$TMPDIR/medium.bin" bs=1K   count=512  2>/dev/null   # 512 KB
    dd if=/dev/urandom of="$TMPDIR/large.bin"  bs=1M   count=5    2>/dev/null   # 5 MB
    dd if=/dev/urandom of="$TMPDIR/xlarge.bin" bs=1M   count=20   2>/dev/null   # 20 MB
    log "  Archivos generados: 10KB, 512KB, 5MB, 20MB"
}

# -------------------------------------------------------------
#  Generar N archivos pequeños para prueba de cantidad
# -------------------------------------------------------------
generar_muchos_archivos() {
    local n=$1
    local dir="$TMPDIR/muchos"
    mkdir -p "$dir"
    for i in $(seq 1 "$n"); do
        dd if=/dev/urandom of="$dir/file_$i.bin" bs=1K count=1 2>/dev/null
    done
    echo "$dir"
}

# =============================================================
#  PRUEBA 1: Latencia de operaciones simples
# =============================================================
prueba_latencia() {
    log ""
    log "============================================="
    log " PRUEBA 1: Latencia de operaciones simples"
    log "============================================="

    $AWS mb s3://bench-latencia > /dev/null 2>&1

    log "--- mb (crear bucket) ---"
    $AWS rb s3://bench-tmp --force > /dev/null 2>&1
    medir "mb" $AWS mb s3://bench-tmp

    log "--- ls (listar buckets) ---"
    medir "ls (sin bucket)" $AWS ls

    log "--- ls (listar objetos) ---"
    $AWS cp "$TMPDIR/small.bin" s3://bench-latencia/small.bin > /dev/null 2>&1
    medir "ls (con bucket)" $AWS ls s3://bench-latencia

    log "--- rb (eliminar bucket vacío) ---"
    medir "rb" $AWS rb s3://bench-tmp

    $AWS rb s3://bench-latencia --force > /dev/null 2>&1
}

# =============================================================
#  PRUEBA 2: Rendimiento de subida (PUT) según tamaño
# =============================================================
prueba_subida() {
    log ""
    log "============================================="
    log " PRUEBA 2: Rendimiento de subida (cp local→s3)"
    log "============================================="

    $AWS mb s3://bench-subida > /dev/null 2>&1

    medir "PUT 10 KB"  $AWS cp "$TMPDIR/small.bin"  s3://bench-subida/small.bin
    medir "PUT 512 KB" $AWS cp "$TMPDIR/medium.bin" s3://bench-subida/medium.bin
    medir "PUT 5 MB"   $AWS cp "$TMPDIR/large.bin"  s3://bench-subida/large.bin
    medir "PUT 20 MB"  $AWS cp "$TMPDIR/xlarge.bin" s3://bench-subida/xlarge.bin

    $AWS rb s3://bench-subida --force > /dev/null 2>&1
}

# =============================================================
#  PRUEBA 3: Rendimiento de descarga (GET) según tamaño
# =============================================================
prueba_descarga() {
    log ""
    log "============================================="
    log " PRUEBA 3: Rendimiento de descarga (cp s3→local)"
    log "============================================="

    $AWS mb s3://bench-descarga > /dev/null 2>&1
    $AWS cp "$TMPDIR/small.bin"  s3://bench-descarga/small.bin  > /dev/null 2>&1
    $AWS cp "$TMPDIR/medium.bin" s3://bench-descarga/medium.bin > /dev/null 2>&1
    $AWS cp "$TMPDIR/large.bin"  s3://bench-descarga/large.bin  > /dev/null 2>&1
    $AWS cp "$TMPDIR/xlarge.bin" s3://bench-descarga/xlarge.bin > /dev/null 2>&1

    medir "GET 10 KB"  $AWS cp s3://bench-descarga/small.bin  "$TMPDIR/dl_small.bin"
    medir "GET 512 KB" $AWS cp s3://bench-descarga/medium.bin "$TMPDIR/dl_medium.bin"
    medir "GET 5 MB"   $AWS cp s3://bench-descarga/large.bin  "$TMPDIR/dl_large.bin"
    medir "GET 20 MB"  $AWS cp s3://bench-descarga/xlarge.bin "$TMPDIR/dl_xlarge.bin"

    $AWS rb s3://bench-descarga --force > /dev/null 2>&1
}

# =============================================================
#  PRUEBA 4: Rendimiento de sync según cantidad de archivos
# =============================================================
prueba_sync() {
    log ""
    log "============================================="
    log " PRUEBA 4: Rendimiento de sync (N archivos × 1KB)"
    log "============================================="

    for n in 10 50 100; do
        local dir
        dir=$(generar_muchos_archivos "$n")
        $AWS rb s3://bench-sync --force > /dev/null 2>&1
        medir "sync $n archivos (local→s3)" $AWS sync "$dir" s3://bench-sync
        rm -rf "$dir"
    done

    $AWS rb s3://bench-sync --force > /dev/null 2>&1
}

# =============================================================
#  PRUEBA 5: mv y rm
# =============================================================
prueba_mv_rm() {
    log ""
    log "============================================="
    log " PRUEBA 5: mv y rm"
    log "============================================="

    $AWS mb s3://bench-mvrem > /dev/null 2>&1
    $AWS cp "$TMPDIR/medium.bin" s3://bench-mvrem/origen.bin > /dev/null 2>&1

    medir "mv (mismo bucket)"   $AWS mv s3://bench-mvrem/origen.bin s3://bench-mvrem/destino.bin
    medir "rm (objeto único)"   $AWS rm s3://bench-mvrem/destino.bin

    # rm --recursive con 20 objetos
    for i in $(seq 1 20); do
        $AWS cp "$TMPDIR/small.bin" s3://bench-mvrem/carpeta/file_$i.bin > /dev/null 2>&1
    done
    medir "rm --recursive (20 objetos)" $AWS rm s3://bench-mvrem/carpeta/ --recursive

    $AWS rb s3://bench-mvrem --force > /dev/null 2>&1
}

# =============================================================
#  PRUEBA 6: sobrescritura de objeto (mismo tamaño vs distinto)
# =============================================================
prueba_sobrescritura() {
    log ""
    log "============================================="
    log " PRUEBA 6: Sobrescritura de objeto"
    log "============================================="

    $AWS mb s3://bench-overwrite > /dev/null 2>&1
    $AWS cp "$TMPDIR/medium.bin" s3://bench-overwrite/obj.bin > /dev/null 2>&1

    # mismo tamaño → escribe en el mismo offset
    dd if=/dev/urandom of="$TMPDIR/medium2.bin" bs=1K count=512 2>/dev/null
    medir "PUT mismo tamaño (overwrite in-place)" \
        $AWS cp "$TMPDIR/medium2.bin" s3://bench-overwrite/obj.bin

    # distinto tamaño → reubica con first-fit
    medir "PUT distinto tamaño (reubicación)" \
        $AWS cp "$TMPDIR/large.bin" s3://bench-overwrite/obj.bin

    $AWS rb s3://bench-overwrite --force > /dev/null 2>&1
}

# =============================================================
#  MAIN
# =============================================================
log "Inicio: $(date)"
log "Cliente: $AWS"
log ""

generar_archivos

prueba_latencia
prueba_subida
prueba_descarga
prueba_sync
prueba_mv_rm
prueba_sobrescritura

log ""
log "Fin: $(date)"
log ""
log "Resultados guardados en: $RESULTS"

# Limpiar temporales
rm -rf "$TMPDIR"
