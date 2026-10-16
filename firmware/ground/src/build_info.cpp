// build_info.cpp — carimbo de compilação, mostrado na linha de boot. BUILD_STAMP_STR vem de
// shared/scripts/build_stamp.py, que o muda a cada build: assim este arquivo é sempre recompilado e o
// carimbo vale mesmo quando só uma biblioteca mudou.
#ifndef BUILD_STAMP_STR
#define BUILD_STAMP_STR __DATE__ " " __TIME__   // sem o script: hora da última compilação deste arquivo
#endif

// extern: sem ele, uma variável const no escopo do arquivo teria ligação interna
extern const char *const BUILD_STAMP = BUILD_STAMP_STR;
