# check_board_mac.py — trava de gravação na placa errada (extra_scripts do PlatformIO)
#
# As duas placas aparecem na mesma COM quando ligadas uma de cada vez, então a
# porta não identifica a placa. Antes de "upload" e "uploadfs", este script lê o
# MAC da placa conectada com o esptool e só deixa seguir se ele for igual ao
# custom_board_mac do ambiente. Nada é apagado nem gravado antes da conferência.
#
# Uso no platformio.ini:
#   extra_scripts    = pre:../shared/scripts/check_board_mac.py
#   custom_board_mac = 1C:69:20:A4:E9:44

import os
import re
import subprocess

Import("env")  # noqa: F821 (injetado pelo SCons do PlatformIO)

ESPERADO = env.GetProjectOption("custom_board_mac", "").strip().upper()  # noqa: F821


def _falha(msg):
    print("\n*** [check_board_mac] " + msg + "\n*** Gravação cancelada.\n")
    env.Exit(1)  # noqa: F821


def confere_mac(source, target, env):
    if not ESPERADO:
        _falha("custom_board_mac não definido em [env:%s]." % env["PIOENV"])

    porta = env.subst("$UPLOAD_PORT")
    if not porta:
        env.AutodetectUploadPort()
        porta = env.subst("$UPLOAD_PORT")
    if not porta:
        _falha("nenhuma porta serial encontrada. A placa está conectada?")

    esptool = os.path.join(
        env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    cmd = [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32",
           "--port", porta, "read_mac"]
    print("[check_board_mac] lendo o MAC da placa em %s..." % porta)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        _falha("o esptool não respondeu em 30 s (porta %s)." % porta)

    m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", r.stdout)
    if r.returncode != 0 or not m:
        print(r.stdout + r.stderr)
        _falha("não foi possível ler o MAC em %s (porta ocupada pelo monitor?)." % porta)

    lido = m.group(1).upper()
    if lido != ESPERADO:
        _falha("placa errada em %s: MAC %s, mas [env:%s] espera %s."
               % (porta, lido, env["PIOENV"], ESPERADO))
    print("[check_board_mac] MAC %s confere com [env:%s]." % (lido, env["PIOENV"]))


env.AddPreAction("upload", confere_mac)  # noqa: F821
env.AddPreAction("uploadfs", confere_mac)  # noqa: F821
