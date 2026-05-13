#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
meshpay_monitor.py — Moniteur serie multi-device pour Mesh Pay.

Se connecte simultanement a plusieurs ESP32 / ESP32-S3 flashes avec la
console de debug (`CONFIG_MESHPAY_DEBUG_CONSOLE=y`) et affiche en temps
reel l'etat du DAG, du wallet, et la convergence entre devices.

Vue d'ensemble TUI (Rich) :

    +----------------------------------------------------+
    | DEVICES                                            |
    | A  Brave-Loup    bal=300  dag=12  locks=0  master  |
    | B  Vif-Renard    bal=120  dag=12  locks=1   -      |
    | C  Calme-Ours    bal=580  dag=11  locks=0   -      |  <- divergence !
    | D  Sage-Cerf     bal=  0  dag=12  locks=0   -      |
    +----------------------------------------------------+
    | CONVERGENCE DAG (intersection de tous les sets)    |
    | tx_id           A  B  C  D                         |
    | abcd1234...     OK OK -- OK                        |  <- C n'a pas vu
    | ef56...         OK OK OK OK                        |
    +----------------------------------------------------+
    | EVENTS                                             |
    | A  16:42:01  LORA_TX broadcast tx=abcd1234         |
    | B  16:42:02  LORA_TX rx tx=abcd1234 merged         |
    | C  16:42:02  -                                     |
    | D  16:42:02  LORA_TX rx tx=abcd1234 merged         |
    +----------------------------------------------------+

Usage :
    pip install -r requirements.txt
    python meshpay_monitor.py \\
        --device A=/dev/cu.usbmodem101 \\
        --device B=/dev/cu.usbmodem201 \\
        --device C=/dev/cu.usbmodem301 \\
        --device D=/dev/cu.usbmodem401

Le label (`A=`, `B=`, ...) est libre, c'est juste pour l'affichage. Si on
omet le `LABEL=`, le port lui-meme sert de label.

Pour lister les ports serie disponibles :
    python -m serial.tools.list_ports

Architecture interne :
  - Un thread par device : lit l'UART ligne par ligne, alimente une
    state-machine de parsing des marqueurs <<<MESHPAY_DEBUG ...>>>.
  - Un thread "poller" : envoie `dump_all\\n` a chaque device toutes les
    N secondes pour rafraichir les snapshots.
  - Le main thread : rendu Rich Live a 4 FPS sur le state agrege.
"""
from __future__ import annotations

import argparse
import json
import re
import signal
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional

# Les dependances Rich et pyserial sont chargees a l'execution pour que
# `--help` et les imports en mode test passent meme sans installation.
try:
    import serial
except ImportError as exc:                                  # pragma: no cover
    serial = None                                           # type: ignore
    _SERIAL_IMPORT_ERR: Optional[Exception] = exc
else:
    _SERIAL_IMPORT_ERR = None


# =====================================================================
# Constantes de protocole
# =====================================================================
#
# Format des marqueurs emis par le firmware (cf. components/debug_console
# /src/debug_console.c). On les detecte avec une regex tolerante a la
# casse pour absorber d'eventuels copy-paste futurs maladroits.

MARKER_BEGIN = re.compile(
    r"<<<MESHPAY_DEBUG\s+(?P<cmd>\S+)\s+BEGIN\s+seq=(?P<seq>\d+)>>>"
)
MARKER_END = re.compile(
    r"<<<MESHPAY_DEBUG\s+(?P<cmd>\S+)\s+END>>>"
)
MARKER_ERR = re.compile(
    r"<<<MESHPAY_DEBUG\s+(?P<cmd>\S+)\s+ERR\s+seq=(?P<seq>\d+)>>>"
)

# Quelques regex pour extraire des events utiles depuis les ESP_LOGI
# du firmware. On ne fait pas de parsing strict — c'est du best-effort
# pour donner du contexte a l'utilisateur. Si le format change, la vue
# "EVENTS" devient juste plus vague, le reste continue de fonctionner.
ESP_LOG_LINE = re.compile(r"^[IWE]\s*\(\s*\d+\s*\)\s*(?P<tag>\S+):\s*(?P<msg>.*)$")


# =====================================================================
# Modele de donnees agrege par device
# =====================================================================

@dataclass
class WalletSnapshot:
    """Snapshot du dump_wallet a un instant donne."""
    own: str = ""
    alias: str = ""
    balance: int = 0
    fee_recipient: str = ""
    last_melt_ts: int = 0
    lock_count: int = 0
    locks: list[dict] = field(default_factory=list)


@dataclass
class DagSnapshot:
    """Snapshot du dump_dag a un instant donne."""
    count: int = 0
    max: int = 0
    # Liste de dicts, dans l'ordre d'insertion. On utilise une liste plutot
    # qu'un set pour preserver l'ordre observe (utile au debug).
    txs: list[dict] = field(default_factory=list)

    @property
    def tx_ids(self) -> set[str]:
        return {tx["id"] for tx in self.txs}


@dataclass
class CurrencySnapshot:
    """Snapshot du dump_currency a un instant donne."""
    name: str = ""
    symbol: str = ""
    decimals: int = 0
    max_supply: int = 0
    transfer_fee: int = 0
    melt_enabled: bool = False
    mint_authority_count: int = 0


@dataclass
class TimeSnapshot:
    """Snapshot du dump_time a un instant donne."""
    mode: str = "?"
    lamport: int = 0
    master_valid: bool = False
    master_offset_ms: int = 0
    master_key: str = ""


@dataclass
class DeviceState:
    """Etat agrege d'un device au sens du moniteur.

    Toutes les ecritures se font sous `lock` car deux threads y touchent
    (le reader serie et le main rendering thread)."""
    label: str
    port: str
    lock: threading.Lock = field(default_factory=threading.Lock)

    connected: bool = False
    last_seen: float = 0.0  # monotonic
    last_error: str = ""

    wallet: WalletSnapshot = field(default_factory=WalletSnapshot)
    dag: DagSnapshot = field(default_factory=DagSnapshot)
    currency: CurrencySnapshot = field(default_factory=CurrencySnapshot)
    time_state: TimeSnapshot = field(default_factory=TimeSnapshot)

    # Buffer circulaire des derniers events ESP_LOGI parses (TAG + msg).
    events: deque[tuple[float, str, str]] = field(
        default_factory=lambda: deque(maxlen=200)
    )

    # Compteurs de raw bytes pour diagnostic.
    bytes_rx: int = 0
    lines_rx: int = 0


# =====================================================================
# Parseur de stream
# =====================================================================
#
# Etat machine simple : on est soit "hors bloc" (les lignes sont des
# logs normaux ESP_LOGI), soit "dans un bloc dump_X" (les lignes sont
# du JSON). Les marqueurs BEGIN/END font transiter.

class StreamParser:
    """Consomme des lignes texte et met a jour un DeviceState.

    Independant du transport serie : testable en passant des chaines."""

    def __init__(self, device: DeviceState) -> None:
        self.device = device
        self._inside_cmd: Optional[str] = None
        self._inside_err = False
        self._json_lines: list[str] = []

    def feed_line(self, raw: str) -> None:
        """Ingurgite UNE ligne (sans \\n final)."""
        line = raw.rstrip("\r\n")
        if not line:
            return

        # Detection des marqueurs.
        m = MARKER_BEGIN.search(line)
        if m:
            self._inside_cmd = m.group("cmd").lower()
            self._inside_err = False
            self._json_lines = []
            return

        m = MARKER_ERR.search(line)
        if m:
            self._inside_cmd = m.group("cmd").lower()
            self._inside_err = True
            self._json_lines = []
            return

        m = MARKER_END.search(line)
        if m:
            self._dispatch_block(
                cmd=m.group("cmd").lower(),
                json_lines=self._json_lines,
                err=self._inside_err,
            )
            self._inside_cmd = None
            self._inside_err = False
            self._json_lines = []
            return

        # Pas un marqueur. Si on est dans un bloc, c'est du JSON.
        if self._inside_cmd is not None:
            self._json_lines.append(line)
            return

        # Sinon c'est un log normal. Best-effort pour extraire un event.
        m = ESP_LOG_LINE.match(line)
        if m:
            with self.device.lock:
                self.device.events.append(
                    (time.time(), m.group("tag"), m.group("msg"))
                )

    def _dispatch_block(self, cmd: str, json_lines: list[str], err: bool) -> None:
        """Dispatch d'un bloc complet sur le bon updater du device."""
        if err:
            # On garde le dernier err pour affichage. Pas de tentative
            # d'updater le snapshot — l'ancien reste visible.
            err_msg = " | ".join(json_lines) or "(no payload)"
            with self.device.lock:
                self.device.last_error = f"{cmd}: {err_msg}"
            return

        if cmd == "dump_dag":
            self._update_dag(json_lines)
        elif cmd == "dump_wallet":
            self._update_wallet(json_lines)
        elif cmd == "dump_currency":
            self._update_currency(json_lines)
        elif cmd == "dump_time":
            self._update_time(json_lines)
        # help et autres : on ignore silencieusement.

    # --- updaters specifiques -----------------------------------------

    def _parse_json_safe(self, s: str) -> Optional[dict]:
        """Parse une ligne JSON, retourne None si invalide.

        On ne plante jamais le moniteur sur du JSON casse — le firmware
        pourrait emettre une ligne tronquee si la TX UART deborde."""
        try:
            return json.loads(s)
        except json.JSONDecodeError:
            return None

    def _update_dag(self, json_lines: list[str]) -> None:
        snapshot = DagSnapshot()
        first = True
        for raw in json_lines:
            obj = self._parse_json_safe(raw)
            if obj is None:
                continue
            if first and "count" in obj and "max" in obj:
                # Premiere ligne : count + max.
                snapshot.count = int(obj.get("count", 0))
                snapshot.max = int(obj.get("max", 0))
                first = False
            else:
                snapshot.txs.append(obj)
        with self.device.lock:
            self.device.dag = snapshot

    def _update_wallet(self, json_lines: list[str]) -> None:
        snapshot = WalletSnapshot()
        first = True
        for raw in json_lines:
            obj = self._parse_json_safe(raw)
            if obj is None:
                continue
            if first and "own" in obj:
                snapshot.own = obj.get("own", "")
                snapshot.alias = obj.get("alias", "")
                snapshot.balance = int(obj.get("balance", 0))
                snapshot.fee_recipient = obj.get("fee_recipient", "")
                snapshot.last_melt_ts = int(obj.get("last_melt_ts", 0))
                snapshot.lock_count = int(obj.get("lock_count", 0))
                first = False
            else:
                snapshot.locks.append(obj)
        with self.device.lock:
            self.device.wallet = snapshot

    def _update_currency(self, json_lines: list[str]) -> None:
        snapshot = CurrencySnapshot()
        for raw in json_lines:
            obj = self._parse_json_safe(raw)
            if obj is None:
                continue
            if "name" in obj:
                snapshot.name = obj.get("name", "")
                snapshot.symbol = obj.get("symbol", "")
                snapshot.decimals = int(obj.get("decimals", 0))
                snapshot.max_supply = int(obj.get("max_supply", 0))
                snapshot.transfer_fee = int(obj.get("transfer_fee", 0))
                snapshot.melt_enabled = bool(obj.get("melt_enabled", False))
                snapshot.mint_authority_count = int(obj.get("mint_authority_count", 0))
        with self.device.lock:
            self.device.currency = snapshot

    def _update_time(self, json_lines: list[str]) -> None:
        snapshot = TimeSnapshot()
        for raw in json_lines:
            obj = self._parse_json_safe(raw)
            if obj is None:
                continue
            if "mode" in obj:
                snapshot.mode = obj.get("mode", "?")
                snapshot.lamport = int(obj.get("lamport", 0))
                snapshot.master_valid = bool(obj.get("master_valid", False))
                snapshot.master_offset_ms = int(obj.get("master_offset_ms", 0))
                snapshot.master_key = obj.get("master_key", "")
        with self.device.lock:
            self.device.time_state = snapshot


# =====================================================================
# Worker thread : lecture serie + envoi periodique
# =====================================================================

class DeviceWorker(threading.Thread):
    """Thread de gestion d'un device : open + read loop + poll dump_all."""

    def __init__(
        self,
        device: DeviceState,
        baud: int,
        poll_interval_s: float,
        stop_event: threading.Event,
    ) -> None:
        super().__init__(name=f"dev-{device.label}", daemon=True)
        self.device = device
        self.baud = baud
        self.poll_interval_s = poll_interval_s
        self.stop_event = stop_event
        self._parser = StreamParser(device)
        self._last_poll = 0.0

    def run(self) -> None:
        if serial is None:                                  # pragma: no cover
            with self.device.lock:
                self.device.last_error = f"pyserial absent: {_SERIAL_IMPORT_ERR}"
            return

        while not self.stop_event.is_set():
            try:
                # Timeout 0.5 s sur les reads = on revient assez souvent
                # pour decider d'envoyer un poll ou de quitter.
                with serial.Serial(
                    self.device.port, baudrate=self.baud, timeout=0.5
                ) as ser:
                    with self.device.lock:
                        self.device.connected = True
                        self.device.last_error = ""
                    self._read_loop(ser)
            except (serial.SerialException, OSError) as exc:
                with self.device.lock:
                    self.device.connected = False
                    self.device.last_error = str(exc)
                # Backoff avant tentative de reconnexion.
                if self.stop_event.wait(2.0):
                    return

    def _read_loop(self, ser: "serial.Serial") -> None:
        """Lit en ligne tant que la connexion tient."""
        buf = b""
        while not self.stop_event.is_set():
            chunk = ser.read(256)
            if chunk:
                with self.device.lock:
                    self.device.bytes_rx += len(chunk)
                    self.device.last_seen = time.monotonic()
                buf += chunk
                # On decoupe sur '\n'. Les lignes incompletes restent en
                # buf jusqu'au prochain read.
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace")
                    with self.device.lock:
                        self.device.lines_rx += 1
                    self._parser.feed_line(text)

            # Poll periodique : declenche dump_all\n.
            now = time.monotonic()
            if now - self._last_poll >= self.poll_interval_s:
                self._last_poll = now
                try:
                    ser.write(b"dump_all\n")
                    ser.flush()
                except (serial.SerialException, OSError) as exc:
                    with self.device.lock:
                        self.device.last_error = f"write: {exc}"
                    raise


# =====================================================================
# Rendu TUI (Rich)
# =====================================================================
#
# On garde le rendu separe du modele : la fonction `render(devices)`
# retourne un Rich renderable et ne mute rien. Facilite les tests.

def _fmt_id(hex_id: str, width: int = 8) -> str:
    """Tronque un id hex pour affichage compact."""
    if len(hex_id) <= width:
        return hex_id
    return hex_id[:width] + "..."


def _fmt_ago(monotonic_ts: float) -> str:
    """Difference avec maintenant en chaine courte."""
    if monotonic_ts == 0.0:
        return "-"
    dt = time.monotonic() - monotonic_ts
    if dt < 1.0:
        return f"{int(dt * 1000)}ms"
    if dt < 60:
        return f"{int(dt)}s"
    return f"{int(dt / 60)}m{int(dt) % 60}s"


def render(devices: list[DeviceState]) -> "object":
    """Construit le rendu Rich global. Import a l'interieur pour que
    le module se charge sans rich (tests)."""
    from rich.table import Table
    from rich.panel import Panel
    from rich.console import Group
    from rich.text import Text

    # --- Panneau 1 : tableau des devices ---------------------------
    devs_tbl = Table(title="Devices", expand=True, show_lines=False)
    devs_tbl.add_column("ID")
    devs_tbl.add_column("Alias")
    devs_tbl.add_column("Balance", justify="right")
    devs_tbl.add_column("DAG", justify="right")
    devs_tbl.add_column("Locks", justify="right")
    devs_tbl.add_column("Mode")
    devs_tbl.add_column("Lamport", justify="right")
    devs_tbl.add_column("Last")
    devs_tbl.add_column("Status")

    for d in devices:
        with d.lock:
            status = "OK" if d.connected and not d.last_error else (
                d.last_error or "?"
            )
            color = "green" if d.connected and not d.last_error else "red"
            devs_tbl.add_row(
                d.label,
                d.wallet.alias or "-",
                f"{d.wallet.balance}",
                f"{d.dag.count}/{d.dag.max}",
                f"{d.wallet.lock_count}",
                d.time_state.mode,
                f"{d.time_state.lamport}",
                _fmt_ago(d.last_seen),
                Text(status[:40], style=color),
            )

    # --- Panneau 2 : convergence DAG -------------------------------
    # On collecte l'union des tx_id vus par tous les devices, puis on
    # remplit la matrice OK / -- selon presence.
    all_ids: dict[str, list[Optional[dict]]] = {}
    snapshots = []
    with_locks = []
    for d in devices:
        with d.lock:
            with_locks.append((d.label, d.dag.txs))
    for label, txs in with_locks:
        for tx in txs:
            all_ids.setdefault(tx["id"], [])
    # On reconstruit en re-iterant pour avoir l'ordre stable.
    sorted_ids = sorted(all_ids.keys())

    conv_tbl = Table(title="Convergence DAG", expand=True)
    conv_tbl.add_column("tx_id")
    for d in devices:
        conv_tbl.add_column(d.label, justify="center")
    conv_tbl.add_column("type")
    conv_tbl.add_column("from->to")
    conv_tbl.add_column("amt", justify="right")

    # Index rapide tx_id -> {label: tx_dict}
    by_id_by_label: dict[str, dict[str, dict]] = {}
    for label, txs in with_locks:
        for tx in txs:
            by_id_by_label.setdefault(tx["id"], {})[label] = tx

    # Limite d'affichage : 30 dernieres TX (sinon ca deborde).
    display_ids = sorted_ids[-30:] if len(sorted_ids) > 30 else sorted_ids
    for tid in display_ids:
        row = [_fmt_id(tid, 12)]
        any_tx = next(iter(by_id_by_label[tid].values()))
        row_class = "green"
        for d in devices:
            tx = by_id_by_label[tid].get(d.label)
            if tx is None:
                row.append(Text("--", style="red"))
                row_class = "yellow"
            else:
                status = tx.get("status", "?")
                glyph = {
                    "CONFIRMED": "OK",
                    "LOCKED": "L",
                    "CANCELLED": "X",
                }.get(status, "?")
                row.append(Text(glyph, style="green" if status == "CONFIRMED"
                                else "yellow" if status == "LOCKED" else "red"))
        row.append(any_tx.get("type", "?"))
        row.append(f"{_fmt_id(any_tx.get('from',''))}->{_fmt_id(any_tx.get('to',''))}")
        row.append(f"{any_tx.get('amount', 0)}")
        conv_tbl.add_row(*row)

    if not display_ids:
        conv_tbl.add_row("(aucune TX visible)", *[""] * (len(devices) + 3))

    # --- Panneau 3 : events recents par device ----------------------
    # Filtre best-effort sur les events qui parlent de LoRa / DAG /
    # attestation / paiement pour rester pertinent.
    evt_tbl = Table(title="Events recents (LoRa / DAG)", expand=True)
    evt_tbl.add_column("Time")
    evt_tbl.add_column("Dev")
    evt_tbl.add_column("Tag")
    evt_tbl.add_column("Message")

    keywords = ("lora", "dag", "attest", "tx", "merge", "ack", "lock",
                "fragment", "sync", "ping", "pong", "broadcast", "mint")

    # On collecte les events de tous les devices avec leur timestamp,
    # on trie par ordre chrono decroissant et on affiche les 20 derniers
    # qui contiennent un mot-cle.
    pool: list[tuple[float, str, str, str]] = []
    for d in devices:
        with d.lock:
            for ts, tag, msg in d.events:
                low = (tag + " " + msg).lower()
                if any(k in low for k in keywords):
                    pool.append((ts, d.label, tag, msg))
    pool.sort(key=lambda x: x[0], reverse=True)
    for ts, label, tag, msg in pool[:20]:
        evt_tbl.add_row(
            datetime.fromtimestamp(ts).strftime("%H:%M:%S"),
            label,
            tag,
            msg[:80],
        )

    if not pool:
        evt_tbl.add_row("-", "-", "-", "(en attente)")

    return Group(
        Panel(devs_tbl, border_style="cyan"),
        Panel(conv_tbl, border_style="magenta"),
        Panel(evt_tbl, border_style="yellow"),
    )


# =====================================================================
# Point d'entree CLI
# =====================================================================

def parse_device_spec(spec: str) -> tuple[str, str]:
    """Parse `LABEL=PORT` ou `PORT`. Retourne (label, port)."""
    if "=" in spec:
        label, _, port = spec.partition("=")
        label = label.strip()
        port = port.strip()
        if not label or not port:
            raise argparse.ArgumentTypeError(
                f"Spec invalide '{spec}' — format attendu LABEL=PORT")
        return label, port
    return spec, spec


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description="Moniteur serie multi-device pour Mesh Pay.")
    p.add_argument(
        "--device", "-d", action="append", required=True, type=parse_device_spec,
        help="LABEL=PORT (peut etre repete). Ex: --device A=/dev/cu.usbmodem101")
    p.add_argument(
        "--baud", "-b", type=int, default=115200,
        help="Baudrate UART (par defaut 115200)")
    p.add_argument(
        "--poll", "-p", type=float, default=5.0,
        help="Intervalle de dump_all en secondes (par defaut 5)")
    p.add_argument(
        "--refresh", type=float, default=4.0,
        help="Frequence de rafraichissement TUI en FPS (par defaut 4)")
    args = p.parse_args(argv)

    if serial is None:
        print(f"Erreur : pyserial non disponible ({_SERIAL_IMPORT_ERR})",
              file=sys.stderr)
        print("Installer avec : pip install -r requirements.txt",
              file=sys.stderr)
        return 2

    try:
        from rich.live import Live
        from rich.console import Console
    except ImportError as exc:
        print(f"Erreur : rich non disponible ({exc})", file=sys.stderr)
        print("Installer avec : pip install -r requirements.txt",
              file=sys.stderr)
        return 2

    devices = [DeviceState(label=label, port=port) for label, port in args.device]

    stop_event = threading.Event()
    workers = [
        DeviceWorker(d, baud=args.baud, poll_interval_s=args.poll,
                     stop_event=stop_event)
        for d in devices
    ]

    # SIGINT propre : on declenche stop_event puis on attend les threads.
    def _on_signal(signum, frame):
        stop_event.set()
    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    for w in workers:
        w.start()

    console = Console()
    refresh_per_second = max(1.0, args.refresh)
    try:
        with Live(render(devices), console=console,
                  refresh_per_second=refresh_per_second,
                  screen=False) as live:
            while not stop_event.is_set():
                live.update(render(devices))
                if stop_event.wait(1.0 / refresh_per_second):
                    break
    finally:
        stop_event.set()
        for w in workers:
            w.join(timeout=3.0)

    return 0


if __name__ == "__main__":                                  # pragma: no cover
    sys.exit(main())
