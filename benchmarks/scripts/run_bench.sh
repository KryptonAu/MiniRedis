#!/bin/bash
# MiniRedis Benchmark Runner
# Usage: ./run_bench.sh [options]
#
# Options:
#   -c N       Concurrency (default: 10)
#   -n N       Requests per test (default: 50000)
#   -m MODE    "full" (18 tests) or "quick" (7 tests, default)
#   -l LABEL   Run label for output filename (default: auto rN)
#   --redis    Also run Redis baseline on port 6380
#   --no-build Skip build step
#   --debug    Use Debug build instead of Release
#   --config PATH
#              Configuration file passed to MiniRedis (default: redis.config)
#   -h         Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_DIR/benchmarks/results/phase_a"
REDIS_BENCH="$PROJECT_DIR/../redis/src/redis-benchmark"
REDIS_SERVER="$PROJECT_DIR/../redis/src/redis-server"
BUILD_DIR="$PROJECT_DIR/build/Release"
MINIREDIS_BIN="$BUILD_DIR/miniredis"
MINIREDIS_PORT=6379
REDIS_PORT=6380
MINIREDIS_CONFIG_FILE="${MINIREDIS_CONFIG_FILE:-$PROJECT_DIR/redis.config}"

# Defaults
CONCURRENCY=10
REQUESTS=50000
MODE="quick"
LABEL=""
DO_REDIS=false
NO_BUILD=false
USE_DEBUG=false

show_help() {
    sed -n '2,15p' "$0"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c) CONCURRENCY="$2"; shift 2 ;;
        -n) REQUESTS="$2"; shift 2 ;;
        -m) MODE="$2"; shift 2 ;;
        -l) LABEL="$2"; shift 2 ;;
        --redis) DO_REDIS=true; shift ;;
        --no-build) NO_BUILD=true; shift ;;
        --debug) USE_DEBUG=true; shift ;;
        --config)
            if [[ $# -lt 2 ]]; then
                echo "Missing path after --config"
                exit 1
            fi
            MINIREDIS_CONFIG_FILE="$2"
            shift 2
            ;;
        -h|--help) show_help; exit 0 ;;
        *) echo "Unknown option: $1"; show_help; exit 1 ;;
    esac
done

# Auto-generate label if not provided
if [[ -z "$LABEL" ]]; then
    existing=$(ls "$RESULTS_DIR"/miniredis_r*.txt 2>/dev/null | wc -l)
    LABEL="r$((existing + 1))"
fi

# Test selection
if [[ "$MODE" == "full" ]]; then
    TESTS="ping_mbulk,set,get,incr,lpush,rpush,lpop,rpop,sadd,hset,spop,zadd,zpopmin,lrange_100,lrange_300,lrange_500,lrange_600,mset"
else
    TESTS="ping_mbulk,set,get,incr,lpush,sadd,lrange_100"
fi

OUTPUT_FILE="$RESULTS_DIR/miniredis_${LABEL}_c${CONCURRENCY}.txt"
REDIS_OUTPUT="$RESULTS_DIR/redis_${LABEL}_c${CONCURRENCY}.txt"

echo "============================================"
echo " MiniRedis Benchmark"
echo "============================================"
echo " Mode:       $MODE ($(echo "$TESTS" | tr ',' '\n' | wc -l) tests)"
echo " Concurrency: $CONCURRENCY"
echo " Requests:   $REQUESTS"
echo " Label:      $LABEL"
echo " Build:      $( $USE_DEBUG && echo Debug || echo Release)"
echo " Config:     $MINIREDIS_CONFIG_FILE"
echo " Output:     $OUTPUT_FILE"
[[ "$DO_REDIS" == true ]] && echo " Redis baseline: yes → $REDIS_OUTPUT"
echo "============================================"

# ---- Build ----
if [[ "$NO_BUILD" == false ]]; then
    echo ""
    echo "[1/3] Building MiniRedis..."
    if $USE_DEBUG; then
        BUILD_DIR="$PROJECT_DIR/build"
        MINIREDIS_BIN="$BUILD_DIR/miniredis"
    fi
    cmake -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 \
        -DCMAKE_BUILD_TYPE=$( $USE_DEBUG && echo Debug || echo Release) \
        > /dev/null 2>&1
    ninja -C "$BUILD_DIR" 2>&1 | tail -1
    echo "Build complete."
else
    echo ""
    echo "[1/3] Skipping build (--no-build)"
fi

# ---- Start MiniRedis ----
echo ""
echo "[2/3] Starting MiniRedis on port $MINIREDIS_PORT..."
if [[ ! -f "$MINIREDIS_CONFIG_FILE" ]]; then
    echo "ERROR: MiniRedis config file not found: $MINIREDIS_CONFIG_FILE"
    exit 1
fi
pkill -9 miniredis 2>/dev/null || true
sleep 1
(cd "$PROJECT_DIR" && "$MINIREDIS_BIN" --config "$MINIREDIS_CONFIG_FILE") \
    > /tmp/miniredis_${LABEL}.log 2>&1 &
sleep 2
if ! ss -tlnp | grep -q ":$MINIREDIS_PORT"; then
    echo "ERROR: MiniRedis failed to start. Check /tmp/miniredis_${LABEL}.log"
    exit 1
fi
echo "MiniRedis is listening on port $MINIREDIS_PORT"

# ---- Run Benchmark ----
echo ""
echo "[3/3] Running benchmark (c=$CONCURRENCY, n=$REQUESTS)..."
"$REDIS_BENCH" -h 127.0.0.1 -p "$MINIREDIS_PORT" \
    -n "$REQUESTS" -c "$CONCURRENCY" -q --precision 3 \
    -t "$TESTS" 2>&1 | tee "$OUTPUT_FILE"

ALIVE=$(pgrep miniredis && echo "YES" || echo "NO")
echo ""
echo "MiniRedis alive after benchmark: $ALIVE"

# ---- Extract summary ----
echo ""
echo "=== Summary ($LABEL, c=$CONCURRENCY) ==="
tr '\r' '\n' < "$OUTPUT_FILE" | awk '/requests per second.*p50=/ && !/rps=/ && NF>3 {
    gsub(/:$/, "", $1); name=$1
    for(i=1;i<=NF;i++){
        if($i ~ /^[0-9]+\.[0-9]+$/ && !seen) {rps=$i; seen=1}
        if($i ~ /p50=/) {gsub(/p50=/, "", $i); gsub(/msec/, "", $i); p50=$i}
    }
    printf "  %-30s %10s rps  p50=%sms\n", name, rps, p50
    seen=0
}'

# ---- Redis baseline (optional) ----
if [[ "$DO_REDIS" == true ]]; then
    echo ""
    echo "=== Redis Baseline ==="
    pkill redis-server 2>/dev/null || true
    cd "$PROJECT_DIR/../redis"
    nohup "$REDIS_SERVER" --port "$REDIS_PORT" --loglevel warning > /dev/null 2>&1 &
    sleep 2
    if ! ss -tlnp | grep -q ":$REDIS_PORT"; then
        echo "WARN: Redis failed to start, skipping baseline"
    else
        echo "Redis is listening on port $REDIS_PORT"
        "$REDIS_BENCH" -h 127.0.0.1 -p "$REDIS_PORT" \
            -n "$REQUESTS" -c "$CONCURRENCY" -q --precision 3 \
            -t "$TESTS" 2>&1 | tee "$REDIS_OUTPUT"
    fi
    pkill redis-server 2>/dev/null || true
fi

# ---- Cleanup ----
echo ""
pkill miniredis 2>/dev/null || true
echo "Done. Results saved to $OUTPUT_FILE"
