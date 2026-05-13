# meshpay-monitor

Moniteur série multi-device pour Mesh Pay. Se connecte simultanément à
plusieurs ESP32 / ESP32-S3 flashés avec la console de debug
(`CONFIG_MESHPAY_DEBUG_CONSOLE=y`) et affiche en temps réel :

- l'état de chaque device (alias, solde, taille DAG, verrous actifs, mode horloge),
- la convergence du DAG entre devices (matrice tx_id × device → OK / --),
- un flux d'events LoRa / DAG / paiement extrait des `ESP_LOGI`.

## Prérequis

- Python 3.10+
- Le firmware doit être buildé avec `CONFIG_MESHPAY_DEBUG_CONSOLE=y` (défaut tant que `SECURE_FLASH_ENCRYPTION_MODE_RELEASE` n'est pas actif).

## Installation

```bash
cd tools/meshpay-monitor
pip install -r requirements.txt
```

## Usage

Lister les ports série disponibles :

```bash
python -m serial.tools.list_ports
```

Lancer le moniteur sur 4 devices :

```bash
python meshpay_monitor.py \
    --device A=/dev/cu.usbmodem101 \
    --device B=/dev/cu.usbmodem201 \
    --device C=/dev/cu.usbmodem301 \
    --device D=/dev/cu.usbmodem401
```

Options :

| Option | Défaut | Rôle |
|---|---|---|
| `--device LABEL=PORT` | (requis) | À répéter pour chaque device |
| `--baud` | 115200 | Baudrate UART |
| `--poll` | 5.0 | Intervalle d'envoi de `dump_all` (secondes) |
| `--refresh` | 4.0 | FPS du rendu TUI |

Quitter : `Ctrl+C`.

## Comment lire l'affichage

1. **Devices** — un coup d'œil rapide : tous les devices doivent voir le même `Mode` (LAMPORT / MASTER), des `Balance` cohérentes, et un `DAG count` qui converge.
2. **Convergence DAG** — pour chaque transaction connue d'au moins un device, on affiche son statut sur chacun (`OK` = CONFIRMED, `L` = LOCKED, `--` = absent). Une colonne `--` persistante = problème de gossip LoRa.
3. **Events** — les derniers `ESP_LOGI` contenant `lora`, `dag`, `attest`, `tx`, `merge`, `ack`, `lock`, `fragment`, `sync`, `ping`, `pong`, `broadcast`, `mint`. Permet de remonter à la séquence d'événements quand quelque chose diverge.

## Limitations connues

- Le poll `dump_all` toutes les 5 s tient `s_state_mutex` ~150 ms (à 250 TX). Si ton scénario implique des paiements toutes les secondes, baisse `--poll` ou retire `dump_dag` du polling.
- Le moniteur n'a pas de vue temps-réel sur la **radio** elle-même (CRC perdus, distance) — il observe ce que le firmware logge. Pour un diagnostic RF complet, un 5ᵉ device en sniffer LoRa passif reste nécessaire.
- Le firmware Waveshare ESP32-S3 est en mode client-only sans LoRa : ses `Events` n'auront pas de lignes LoRa, c'est attendu.

## Tests

```bash
python -m unittest discover -s tests
```

Couvre la state-machine de parsing des marqueurs et l'extraction
des snapshots DAG / wallet / currency / time.
