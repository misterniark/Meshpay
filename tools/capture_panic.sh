#!/usr/bin/env bash
#
# capture_panic.sh
#
# Capture le log serie d'un device Mesh Pay (ESP32-S3 Waveshare) et
# decode automatiquement la backtrace au prochain "Guru Meditation" /
# panic. Symbolise via addr2line contre l'ELF du build courant.
#
# Usage :
#   ./tools/capture_panic.sh [port]
#
#   port : chemin du device (defaut /dev/cu.usbmodem101). Pour le 2e
#          device sur le meme Mac, typiquement /dev/cu.usbmodem2101.
#
# Pour quitter la capture pendant qu'elle tourne : Ctrl+T (ne pas
# Ctrl+C tant que le device tourne, sinon le log n'est pas decode).
# Apres avoir reproduit le crash, terminer la capture : Ctrl+]
# (raccourci idf.py monitor) puis le script affiche la symbolisation.
#
# Pre-requis :
#   - ESP-IDF source (export.sh) ou variable IDF_PATH definie
#   - build/ existant (idf.py build prealable)
#

set -u

PORT="${1:-/dev/cu.usbmodem101}"

# Localiser l'ELF du build courant.
ELF="build/offline-payment.elf"
if [[ ! -f "$ELF" ]]; then
    echo "ERREUR : $ELF introuvable. Lancer 'idf.py build' avant." >&2
    exit 1
fi

# Localiser addr2line. L'outil exact depend du target ; on cherche
# d'abord la version esp-elf moderne (esp-14.x), sinon la version
# specifique au target.
ADDR2LINE=""
for cand in \
    "$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20250730/xtensa-esp-elf/bin/xtensa-esp-elf-addr2line" \
    "$(which xtensa-esp32s3-elf-addr2line 2>/dev/null)" \
    "$(which xtensa-esp-elf-addr2line 2>/dev/null)" ; do
    if [[ -n "$cand" && -x "$cand" ]]; then
        ADDR2LINE="$cand"
        break
    fi
done
if [[ -z "$ADDR2LINE" ]]; then
    echo "ERREUR : addr2line introuvable. Sourcer export.sh d'ESP-IDF." >&2
    exit 1
fi

# Fichier de log horodate.
LOG="/tmp/meshpay_panic_$(date +%Y%m%d_%H%M%S).log"
echo "Capture demarree -> $LOG"
echo "Port : $PORT"
echo "ELF  : $ELF"
echo "addr2line : $ADDR2LINE"
echo ""
echo "Reproduire le crash (paiement entre 2 devices), puis Ctrl+] pour"
echo "sortir du monitor et lancer la symbolisation automatique."
echo ""

# Capture via idf.py monitor (qui fait deja addr2line en temps reel
# en theorie, mais on garde une copie brute pour analyse offline).
idf.py -p "$PORT" monitor 2>&1 | tee "$LOG"

# Apres sortie du monitor, extraire toutes les adresses 0x40xxxxxx
# (IRAM/Flash code) et les symboliser. On filtre celles deja decodees.
echo ""
echo "==============================="
echo " Symbolisation post-capture"
echo "==============================="
ADDRS=$(grep -oE '0x4[0-9a-f]{7}' "$LOG" | sort -u)
if [[ -z "$ADDRS" ]]; then
    echo "Aucune adresse 0x4xxxxxxx trouvee dans le log."
    exit 0
fi
echo "$ADDRS" | tr '\n' ' '
echo ""
echo ""
"$ADDR2LINE" -pfiaC -e "$ELF" $ADDRS

echo ""
echo "Log brut conserve : $LOG"
