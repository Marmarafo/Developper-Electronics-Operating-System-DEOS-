#ifndef DEOS_APPS_H
#define DEOS_APPS_H
#include "console.h"

// Masaustu uygulamalari (hepsi kernel icinde, wm.c'nin pencere API'sini kullanir).
void app_terminal_open(void);                 // shell'in calistigi terminal; konsolu bu pencereye yonlendirir
void app_monitor_open(void);                  // sistem izleyici (CPU grafigi, bellek, thread'ler)
void app_notepad_open(void);
void app_notepad_open_file(const char *abs_path);   // dosyayi yukleyerek acar
void app_files_open(void);                           // dosya yoneticisi
void app_calc_open(void);
void app_about_open(void);

#endif
