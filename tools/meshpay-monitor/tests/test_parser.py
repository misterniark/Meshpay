# -*- coding: utf-8 -*-
"""
Tests du StreamParser de meshpay_monitor.

Verifie que la state-machine de parsing des marqueurs <<<MESHPAY_DEBUG
... BEGIN/END/ERR>>> reconstruit correctement les snapshots DAG /
wallet / currency / time a partir d'un flux de lignes brutes — y
compris quand des ESP_LOGI s'intercalent.

Independant du transport serie (on n'instancie pas DeviceWorker).
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

# Le module n'est pas un package : on ajoute le parent au sys.path
# avant d'importer.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from meshpay_monitor import (
    DeviceState,
    StreamParser,
    parse_device_spec,
)


def feed(parser: StreamParser, lines: list[str]) -> None:
    """Helper : alimente le parser ligne par ligne (sans \\n final)."""
    for line in lines:
        parser.feed_line(line)


class StreamParserTests(unittest.TestCase):

    def setUp(self) -> None:
        self.device = DeviceState(label="A", port="/dev/null")
        self.parser = StreamParser(self.device)

    def test_dump_dag_reconstruit_count_et_txs(self) -> None:
        """Un bloc dump_dag complet doit remplir DagSnapshot."""
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_dag BEGIN seq=1>>>",
            '{"count":2,"max":250}',
            '{"i":0,"id":"aabb","type":"MINT","from":"00","to":"11",'
            '"amount":1000,"currency":1,"fee":0,"seq":1,'
            '"status":"CONFIRMED","ts":12345,"parents":[]}',
            '{"i":1,"id":"ccdd","type":"TRANSFER","from":"11","to":"22",'
            '"amount":50,"currency":1,"fee":0,"seq":1,'
            '"status":"LOCKED","ts":12346,"parents":["aabb"]}',
            "<<<MESHPAY_DEBUG dump_dag END>>>",
        ])
        self.assertEqual(self.device.dag.count, 2)
        self.assertEqual(self.device.dag.max, 250)
        self.assertEqual(len(self.device.dag.txs), 2)
        self.assertEqual(self.device.dag.tx_ids, {"aabb", "ccdd"})

    def test_logs_intercales_pendant_un_dump_sont_traites_comme_json(
            self) -> None:
        """Si un ESP_LOGI passe entre BEGIN et END, il est consomme comme
        ligne JSON et discrete au parsing (JSON invalide ignore).

        C'est ce que voit le monitor en pratique sur l'UART partagee :
        ESP_LOG continue d'ecrire. La regle "tout ce qui est entre
        BEGIN/END est candidat JSON" tient parce que json.loads sur un
        log "I (123) tag: msg" echoue silencieusement (ligne ignoree)."""
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_dag BEGIN seq=1>>>",
            '{"count":1,"max":250}',
            "I (12345) lora_sync: tx send",  # log intercale
            '{"i":0,"id":"ff00","type":"MINT","from":"00","to":"11",'
            '"amount":100,"currency":1,"fee":0,"seq":1,'
            '"status":"CONFIRMED","ts":1,"parents":[]}',
            "<<<MESHPAY_DEBUG dump_dag END>>>",
        ])
        self.assertEqual(self.device.dag.count, 1)
        self.assertEqual(len(self.device.dag.txs), 1)

    def test_dump_wallet_capture_alias_et_locks(self) -> None:
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_wallet BEGIN seq=2>>>",
            '{"own":"abcd","alias":"Brave-Loup",'
            '"balance":300,"fee_recipient":"0000",'
            '"last_melt_ts":0,"lock_count":1,"max_locks":10}',
            '{"i":0,"tx_id":"deadbeef","amount":50,"lock_time":100}',
            "<<<MESHPAY_DEBUG dump_wallet END>>>",
        ])
        self.assertEqual(self.device.wallet.alias, "Brave-Loup")
        self.assertEqual(self.device.wallet.balance, 300)
        self.assertEqual(self.device.wallet.lock_count, 1)
        self.assertEqual(len(self.device.wallet.locks), 1)
        self.assertEqual(self.device.wallet.locks[0]["tx_id"], "deadbeef")

    def test_dump_time_capture_mode_master(self) -> None:
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_time BEGIN seq=3>>>",
            '{"mode":"MASTER","lamport":42,"master_valid":true,'
            '"master_offset_ms":-150,"last_master_update":999,'
            '"master_key":"deadbeef"}',
            "<<<MESHPAY_DEBUG dump_time END>>>",
        ])
        self.assertEqual(self.device.time_state.mode, "MASTER")
        self.assertEqual(self.device.time_state.lamport, 42)
        self.assertTrue(self.device.time_state.master_valid)
        self.assertEqual(self.device.time_state.master_offset_ms, -150)

    def test_dump_currency_capture_nom_symbole_decimales(self) -> None:
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_currency BEGIN seq=4>>>",
            '{"id":1,"name":"Festival2026","symbol":"FC","decimals":2,'
            '"max_supply":100000,"valid_until":0,"initial_balance":1000,'
            '"transfer_fee":1,"melt_enabled":true,"melt_period":86400,'
            '"melt_mode":"BPS","melt_bps":100,"melt_fixed":0,'
            '"mint_authority_count":1}',
            '{"i":0,"pubkey":"abcd1234"}',
            "<<<MESHPAY_DEBUG dump_currency END>>>",
        ])
        self.assertEqual(self.device.currency.name, "Festival2026")
        self.assertEqual(self.device.currency.symbol, "FC")
        self.assertEqual(self.device.currency.decimals, 2)
        self.assertTrue(self.device.currency.melt_enabled)
        self.assertEqual(self.device.currency.mint_authority_count, 1)

    def test_trame_ERR_capture_le_message_dans_last_error(self) -> None:
        """Si un dump retourne ERR (mutex_timeout, not_implemented), on
        ne touche pas au snapshot existant et on note last_error."""
        self.device.dag.count = 5  # snapshot "ancien"
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_dag ERR seq=7>>>",
            '{"err":"mutex_timeout"}',
            "<<<MESHPAY_DEBUG dump_dag END>>>",
        ])
        self.assertEqual(self.device.dag.count, 5)  # inchange
        self.assertIn("mutex_timeout", self.device.last_error)

    def test_blocs_imbriques_sont_traites_sequentiellement(self) -> None:
        """dump_all emet 4 blocs successifs. Verifie que chaque bloc
        rafraichit son propre snapshot."""
        feed(self.parser, [
            "<<<MESHPAY_DEBUG dump_dag BEGIN seq=10>>>",
            '{"count":3,"max":250}',
            "<<<MESHPAY_DEBUG dump_dag END>>>",
            "<<<MESHPAY_DEBUG dump_wallet BEGIN seq=11>>>",
            '{"own":"a","alias":"X","balance":7,"fee_recipient":"0",'
            '"last_melt_ts":0,"lock_count":0,"max_locks":10}',
            "<<<MESHPAY_DEBUG dump_wallet END>>>",
        ])
        self.assertEqual(self.device.dag.count, 3)
        self.assertEqual(self.device.wallet.balance, 7)

    def test_log_normal_hors_bloc_est_capture_comme_event(self) -> None:
        feed(self.parser, [
            "I (12345) lora_sync: LORA_TX tx_id=abcdef merged",
        ])
        # Au moins un event a ete capture.
        self.assertEqual(len(self.device.events), 1)
        ts, tag, msg = list(self.device.events)[0]
        self.assertEqual(tag, "lora_sync")
        self.assertIn("LORA_TX", msg)

    def test_ligne_vide_ne_plante_pas(self) -> None:
        feed(self.parser, ["", "  ", "\r"])  # pas d'exception


class ParseDeviceSpecTests(unittest.TestCase):

    def test_format_label_egal_port(self) -> None:
        self.assertEqual(parse_device_spec("A=/dev/cu.usbmodem01"),
                         ("A", "/dev/cu.usbmodem01"))

    def test_port_seul_label_egale_port(self) -> None:
        self.assertEqual(parse_device_spec("/dev/cu.usbmodem01"),
                         ("/dev/cu.usbmodem01", "/dev/cu.usbmodem01"))

    def test_spec_invalide_egal_vide_leve_exception(self) -> None:
        import argparse as _ap
        with self.assertRaises(_ap.ArgumentTypeError):
            parse_device_spec("=foo")
        with self.assertRaises(_ap.ArgumentTypeError):
            parse_device_spec("foo=")


if __name__ == "__main__":
    unittest.main()
