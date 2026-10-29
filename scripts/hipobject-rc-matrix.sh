#!/bin/bash
# Copyright 2026 DataDirect Networks, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Correctness matrix for the hipObject (hipobj-rc-v2) RDMA flavor.
#
# GET: objects are uploaded over plain HTTPS, so they carry MD5 ETags, then
#      fetched with --flavor $FLAVOR --verify.
# PUT: objects are uploaded with --flavor $FLAVOR --verify, downloaded over plain
#      HTTPS, and compared with the payload MD5 the client logs. The check
#      does not rely on the ETag, because the server assigns RDMA PUTs their
#      own (reds3 issues UUID-style ones).
#
# Environment:
#   AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY  credentials (required)
#   ENDPOINT     S3 endpoint on the RDMA address, e.g. https://172.16.1.100:8111
#   RC_DEVICE    verbs device, e.g. rocep6s0 (default: first device)
#   FLAVOR       amd (default) or nvidia, which runs the same checks on cuObject
#   GPU          GPU device ID: transfer to and from GPU memory (default: host memory)
#   BUCKET       bucket to use (default: rc-test)
#   CLIENT       path to simple_s3_rdma (default: ./build/simple_s3_rdma)
#   SIZES        object sizes (default: "4KiB 1MiB 16MiB 256MiB")
#   THREADS      thread counts (default: "1 4")
#   PER_THREAD   objects per thread (default: 2)
#
# Needs the aws CLI. Exits non-zero if any transfer or comparison fails.

: "${AWS_ACCESS_KEY_ID:?}" "${AWS_SECRET_ACCESS_KEY:?}" "${ENDPOINT:?set ENDPOINT}"
BUCKET=${BUCKET:-rc-test}
CLIENT=${CLIENT:-./build/simple_s3_rdma}
SIZES=${SIZES:-"4KiB 1MiB 16MiB 256MiB"}
THREADS=${THREADS:-"1 4"}
PER=${PER_THREAD:-2}
FLAVOR=${FLAVOR:-amd}

A=(aws --endpoint-url "$ENDPOINT" --no-verify-ssl --region us-east-1)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

bytes() {
    case $1 in
    *KiB) echo $((${1%KiB} * 1024)) ;;
    *MiB) echo $((${1%MiB} * 1048576)) ;;
    *GiB) echo $((${1%GiB} * 1073741824)) ;;
    *) echo "$1" ;;
    esac
}

client() {
    local op=$1 size=$2 threads=$3
    "$CLIENT" "$op" --flavor "$FLAVOR" ${RC_DEVICE:+--rc-device "$RC_DEVICE"} ${GPU:+--gpu "$GPU"} \
        --endpoint "$ENDPOINT" --access-key "$AWS_ACCESS_KEY_ID" \
        --secret-key "$AWS_SECRET_ACCESS_KEY" --bucket-name "$BUCKET" --verify \
        --obj.size "$size" --count "$PER" --threads "$threads" 2>&1
}

fail=0
for size in $SIZES; do
    n=$(bytes "$size")
    for threads in $THREADS; do
        total=$((PER * threads))

        # GET: the client reads test_<thread>_<bytes>_<i>.bin
        for t in $(seq 0 $((threads - 1))); do
            for i in $(seq 0 $((PER - 1))); do
                k=test_${t}_${n}_${i}.bin
                head -c "$n" /dev/urandom >"$T/$k"
                "${A[@]}" s3 cp "$T/$k" "s3://$BUCKET/$k" --quiet 2>/dev/null ||
                    echo "  upload of $k failed"
                rm -f "$T/$k"
            done
        done
        out=$(client get "$size" "$threads")
        ok=$(sed -n 's/.*Successful operations: //p' <<<"$out")
        echo "GET $size x$total threads=$threads: ${ok:-0}/$total ok"
        if [ "$ok" != "$total" ]; then
            fail=1
            grep error <<<"$out" | head -3
        fi

        # PUT: --verify logs the payload MD5 of every PUT sent over RDMA
        out=$(client put "$size" "$threads")
        ok=$(sed -n 's/.*Successful operations: //p' <<<"$out")
        if [ "$ok" != "$total" ]; then
            echo "  PUT $size threads=$threads: ${ok:-0}/$total transfers completed"
            fail=1
            grep error <<<"$out" | head -3
        fi
        bad=0
        for t in $(seq 0 $((threads - 1))); do
            for i in $(seq 0 $((PER - 1))); do
                k=test_${t}_${n}_${i}.bin
                want=$(grep -F -- "--verify: $k was written over RDMA" <<<"$out" | head -1 |
                    sed -n 's/.*payload MD5 \([0-9a-f]*\).*/\1/p')
                got=$("${A[@]}" s3 cp "s3://$BUCKET/$k" - 2>/dev/null | md5sum | cut -d' ' -f1)
                if [ -z "$want" ]; then
                    # The PUT fell back to TCP, so its ETag is the MD5
                    want=$("${A[@]}" s3api head-object --bucket "$BUCKET" --key "$k" \
                        --query ETag --output text 2>/dev/null | tr -d '"')
                fi
                if [ -z "$want" ] || [ "$want" != "$got" ]; then
                    bad=$((bad + 1))
                    echo "  PUT $k: client buffer $want, stored $got"
                fi
            done
        done
        echo "PUT $size x$total threads=$threads: $((total - bad))/$total match over HTTPS"
        [ $bad = 0 ] || fail=1
    done
done
exit $fail
