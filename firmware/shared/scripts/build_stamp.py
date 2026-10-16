# build_stamp.py — carimbo de compilação (extra_scripts do PlatformIO)
#
# Passa a hora do build como BUILD_STAMP_STR só para src/build_info.cpp. Como a linha de comando dele
# muda a cada build, o SCons (que decide pela assinatura do conteúdo e do comando, e não pela data do
# arquivo) sempre o recompila, e o carimbo vale mesmo quando só uma biblioteca mudou. Os outros
# arquivos não são afetados. A linha de boot mostra o carimbo: um teste feito com o firmware antigo
# (gravação que falhou) fica evidente no log.
#
# Uso no platformio.ini:
#   extra_scripts = pre:../shared/scripts/build_stamp.py

import time

Import("env")  # noqa: F821 (injetado pelo SCons do PlatformIO)

CARIMBO = time.strftime("%Y-%m-%d %H:%M:%S")


def com_carimbo(env, node):
    defines = list(env.get("CPPDEFINES", [])) + [("BUILD_STAMP_STR", env.StringifyMacro(CARIMBO))]
    return env.Object(node, CPPDEFINES=defines)


env.AddBuildMiddleware(com_carimbo, "*build_info.cpp")  # noqa: F821
