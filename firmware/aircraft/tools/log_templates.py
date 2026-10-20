"""Compara os modelos de linha de logs do monitor serial (docs/refatoracao.md, passo 8).

Troca cada número por # e junta os espaços, para comparar o texto das mensagens sem os valores.
Mostra os modelos que só aparecem de um lado, com quantas vezes e um exemplo:
  - só no novo: mensagem nova ou com texto diferente;
  - só na base: mensagem que não apareceu no novo (esperado quando o teste não passou por ela).
As linhas da ROM de boot do ESP32 ficam de fora.

Alcance: compara conjuntos de modelos. Não confere a ordem das linhas nem acusa mudança de
frequência de um modelo que existe dos dois lados. Ajuda a revisar o texto, mas não comprova
sozinho que a sequência das mensagens foi preservada: a ordem se confere lendo o log.

Uso (a partir de firmware/aircraft):
    python tools/log_templates.py logs/base.log                       # novo = último log em logs/ fora das bases
    python tools/log_templates.py logs/base1.log logs/base2.log       # base com os modelos dos dois
    python tools/log_templates.py logs/base.log --novo logs/novo.log
"""
import argparse
import collections
import glob
import os
import re
import sys

ROM_PREFIXES = ("ets ", "rst:", "configsip:", "clk_drv:", "mode:", "load:", "entry ")
NUMBER = re.compile(r"0x[0-9A-Fa-f]+|[+-]?\d+(?:[.,]\d+)?")


def templates(paths):
    counts = collections.Counter()
    example = {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                if not line.strip() or line.startswith(ROM_PREFIXES):
                    continue
                t = re.sub(r"\s+", " ", NUMBER.sub("#", line)).strip()
                counts[t] += 1
                example.setdefault(t, line.strip())
    return counts, example


def report(title, only, counts, example):
    print(f"\n{title}: {len(only)}")
    for t in sorted(only):
        print(f"  {counts[t]:5d}x  {example[t][:160]}")


def same_file(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def main():
    # O console do Windows usa cp1252 e não imprime Δ nem µ (stderr: mensagens de erro do argparse)
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Compara os modelos de linha (números trocados por #) de um log novo com os de logs de base.")
    parser.add_argument("bases", nargs="+", metavar="BASE", help="log(s) de base")
    parser.add_argument("--novo", metavar="LOG",
                        help="log novo (padrão: o mais recente em logs/ que não seja uma das bases)")
    args = parser.parse_args()

    for path in args.bases + ([args.novo] if args.novo else []):
        if not os.path.isfile(path):
            parser.error(f"arquivo não encontrado: {path}")

    if args.novo:
        new = args.novo
        if any(same_file(new, b) for b in args.bases):
            parser.error(f"o log novo também está entre as bases: {new}")
    else:
        logs = [p for p in glob.glob(os.path.join("logs", "*.log"))
                if not any(same_file(p, b) for b in args.bases)]
        if not logs:
            parser.error("nenhum log em logs/ fora das bases: indique o novo com --novo")
        new = max(logs, key=os.path.getmtime)

    cb, eb = templates(args.bases)
    cn, en = templates([new])
    print(f"base: {', '.join(args.bases)} ({len(cb)} modelos)")
    print(f"novo: {new} ({len(cn)} modelos)")
    report("Só no novo", set(cn) - set(cb), cn, en)
    report("Só na base", set(cb) - set(cn), cb, eb)


if __name__ == "__main__":
    main()
