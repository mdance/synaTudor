#!/bin/bash -e

HASH_FILE="$1"
TMP_DIR="$2"
OUT_DIR="$3"
DLLS=${@:4}

mkdir -p "$TMP_DIR"

#Use locally extracted DLLs if TUDOR_DRIVER_DIR is set (e.g. pulled from a Windows install)
if [ -n "$TUDOR_DRIVER_DIR" ]; then
    mkdir -p "$OUT_DIR"
    for dll in $DLLS
    do
        cp "$TUDOR_DRIVER_DIR/$dll" "$OUT_DIR/$dll"
    done
    exit 0
fi

#Download the driver executable and check hash
INSTALLER="$TMP_DIR/installer.exe"
curl -L https://ftp.hp.com/pub/softpaq/sp138001-138500/sp138227.exe -o "$INSTALLER"
shasum "$INSTALLER" | cut -d" " -f1 | cmp - "$HASH_FILE"

#Extract the driver
WINDRV="$TMP_DIR/windrv"
mkdir -p "$WINDRV"
7z x "$INSTALLER" -o"$WINDRV"

#Copy outputs
mkdir -p "$OUT_DIR"
for dll in $DLLS
do
    cp $(find "$WINDRV" -name "$dll") "$OUT_DIR/$dll"
done