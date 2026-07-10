# libfb-vt

Консольная графика через framebuffer и зародыш display-сервера для FreeBSD 10+ (vt(4)).
Только нативные интерфейсы FreeBSD — никаких `/dev/mem`, kvm, DRM/KMS или linuxkpi.
Используются лишь `<sys/fbio.h>`, `<sys/consio.h>`, `<sys/kbio.h>`.

## Слои

- `fb.c` / `fb.h`     — framebuffer: открытие + `mmap` через `FBIOGTYPE`, программная двойная буферизация.
- `ppm.c` / `ppm.h`   — загрузчик PPM (P6/P3, комментарии, любой maxval) + 32bpp blit/cls.
- `vtcon.c` / `vtcon.h` — нативное владение VT/консолью: захват через `VT_OPENQRY`, `KD_GRAPHICS`
  и рукопожатие переключения `VT_SETMODE(VT_PROCESS)` (тот же механизм, что использует X).
- `mouse.c` / `mouse.h` — курсор мыши: читает `/dev/sysmouse` (нужен запущенный `moused(8)`),
  декодирует относительные dx/dy/колесо/кнопки в протоколе sysmouse (уровень 1, 8 байт, с
  откатом на уровень 0), копит абсолютную позицию с клипом по экрану.
- `kbd.c` / `kbd.h` — декодер сырых скан-кодов клавиатуры (`K_CODE`, AT/PS2 set 1) с
  реальным состоянием Shift/Ctrl/Alt — опционально (`server -k`), нужен только для комбинаций
  вроде Shift+стрелка, которые `K_XLATE` не может выразить (см. раздел «Клавиатура» ниже).
- `proto.c` / `proto.h` — протокол display-сервера: кадрированные сообщения поверх
  `AF_UNIX`-сокета + передача fd через `SCM_RIGHTS` (для разделяемого буфера пикселей).
- `fontspleen.h`      — встроенный моноширинный шрифт 8×16 (Spleen, BSD-2-Clause), проиндексированный
  по Unicode: ASCII + Latin-1 + кириллица + псевдографика + блочные символы. Поиск глифа — `glyph_for(cp)`.

## Программы

- `ppm2fb image.ppm`        — одноразово: нарисовать изображение один раз на текущей консоли.
- `fbshow  image.ppm [vt]`  — постоянный просмотрщик (опрос; захватывает свободный VT).
- `server  [-e команда] [-w обои.ppm] [-k] [vt]` — **компоновщик (compositor)**: эксклюзивно
  владеет VT, слушает `AF_UNIX`-сокет, выдаёт каждому клиенту разделяемый буфер пикселей на
  «поверхность» (окно), композитит все окна (фон рабочего стола + рамка + заголовок + пиксели
  клиента) и раздаёт ввод с клавиатуры сфокусированному (верхнему) окну. `-e команда` — после
  захвата VT и поднятия сокета запустить `команду` (через `/bin/sh -c`) как первого клиента.
  `-k` — читать клавиатуру как сырые `K_CODE`-скан-коды вместо `K_XLATE` (см. «Клавиатура»
  ниже); по умолчанию выключено.
- `term    [rows] [cols]`   — **графический терминал** как клиент компоновщика: своего VT/framebuffer
  не имеет, общается по протоколу, получает разделяемый буфер под окно, форкает shell в pty
  (`forkpty`) и эмулирует VT100-подобный терминал в этот буфер встроенным шрифтом 8×8.
  Размер по умолчанию 24×80, обрезается под выданный размер. Собирается один бинарник,
  не зависящий от бэкенда (линкуется с `-lutil`).
- `cube    [vt]`            — демо: вращающийся сплошь затенённый 3D-куб в окне 320x200
  (тот же скелет сервера; программный рендерер, работает на любом бэкенде). Требует `-lm`.

`[vt]` — это номер VT с отсчётом от 1 (7 == ttyv6); опустите или укажите 0, чтобы автоматически выбрать первый свободный VT.

## Сборка

    make
    # или, например, только клиент терминала:
    make term
    # server напрямую (выберите бэкенд — .svga / .vga / .fb):
    make server.svga

## Запуск: компоновщик + терминал

Требуется root, на консоли vt(4) (`sysctl kern.vty` должен сообщать `vt`).

Компоновщик монопольно захватывает VT, поэтому запустить клиент «руками» с той же
консоли уже не выйдет — на заблокированном VT нет доступного shell. Два рабочих способа:

    # 1) пусть сервер сам поднимет первый терминал (проще всего):
    sudo ./server.svga -e ./term

    # 2) или запустить клиента отдельным процессом в той же командной строке:
    sudo sh -c './server.svga & sleep 1; ./term; wait'

    # обои на весь экран (PPM, растягивается nearest-neighbour):
    sudo ./server.svga -w ./wall.ppm -e ./term

Клавиатура уходит в верхнее (сфокусированное) окно; всё, что печатаете, попадает в shell.
Компоновщик завершается по `SIGINT`/`SIGTERM`/`SIGHUP` **или** автоматически, когда отключился
последний подключавшийся клиент (например, вы набрали `exit` в терминале). `server` отказывается
переключать VT (`Alt+Fn`, `Ctrl+Alt+Fn`, `Alt+стрелки`) во время работы; см. раздел ниже.

### Мышь

При старте компоновщик пытается открыть `/dev/sysmouse` (нужен запущенный `moused(8)`,
например `moused -p /dev/psm0 -t auto`); если устройство недоступно, сервер просто
работает без указателя — клавиатура не страдает. Когда мышь есть:

- курсор рисуется поверх композиции (стрелка с чёрной окантовкой);
- клик по окну поднимает его наверх и делает сфокусированным;
- зажатие ЛКМ на заголовке окна перетаскивает его (клип в границах экрана);
- в правом углу заголовка — кнопка системного меню (три полоски); ЛКМ по ней
  открывает меню окна с пунктами «Close» (посылает клиенту `FBVT_CLOSE`,
  клиент сам закрывает сокет) и «Send to back» (опускает окно в самый низ
  z-порядка); клик мимо пунктов меню просто закрывает его;
- пока курсор над содержимым сфокусированного окна, движения/клики/колесо уходят
  клиенту сообщением `FBVT_INPUT_MOUSE` (координаты в пикселях, относительно окна).

`term` транслирует эти события в стандартные xterm-последовательности отчёта мыши
(`DECSET 1000`/`1002` + `1006` SGR-координаты), так что приложения в шелле (`less`,
`vim`, `htop`, …), включившие мышь сами, увидят клики как обычно. Если приложение
мышь не включало (обычный shell-промпт), колесо вместо этого листает собственный
scrollback терминала — до 2000 строк, ушедших с экрана на основном (не alt-screen)
буфере; любое нажатие клавиши возвращает к живому выводу.

### Клавиатура

По умолчанию клавиатура читается в `K_XLATE` — ядро само переводит скан-коды в
ASCII/ANSI-последовательности (буквы, Backspace, Enter, стрелки, F1–F12 всё это уже
корректно доезжает до `term`). Ограничение `K_XLATE`: он не умеет выражать модификатор
у специальных клавиш — Shift+стрелка на клавиатуре и без Shift шлют **один и тот же**
байтовый поток, поэтому выделение текста в mc/vim (`Shift+↑/↓/←/→`) в таком режиме
принципиально не различимо на нашей стороне.

`server -k` переключает клавиатуру в `K_CODE` (сырые скан-коды, `kbd.c`) и сам отслеживает
состояние Shift/Ctrl/Alt, синтезируя правильные xterm-последовательности с модификатором
(`ESC[1;2A` для Shift+Up и т.п.). Таблица скан-кодов сверена с `KBD_DEBUG=1`-логами на
реальном железе: буквы/цифры/пунктуация/Enter/Backspace/Esc/Tab/F1–F12 — стандартный
AT/PS2 set 1, а вот стрелки/Home/End/PgUp/PgDn/Ins/Del оказались НЕ классической схемой
с префиксом `0xE0` (как можно было бы ожидать) — эта консоль отдаёт их напрямую, плоским
диапазоном кодов `0x5E–0x67`, без всякого префикса. Путь с `0xE0` оставлен в `kbd.c` как
запасной на случай другой клавиатуры/драйвера, но на проверенном железе не срабатывает.

Если что-то всё же ведёт себя не так (`server` запускался без `-k` до этого — по умолчанию
ничего не меняется):

    sudo env KBD_DEBUG=1 ./server.vga -k 2>/tmp/kbd.log

В `/tmp/kbd.log` будут строки `sc 0x1e -> 1 byte` на каждое нажатие — если буквы/цифры/
стрелки/Enter/Backspace работают некорректно с `-k`, пришлите этот лог для правки таблицы
в `kbd.c`. Без `-k` ничего не меняется — это чисто опциональный режим.

### Чёрный экран вместо рабочего стола?

Компоновщик рисует фон сразу при старте, поэтому мгновенный чёрный экран обычно значит,
что `server` **уже вышел**: он завершается, как только отключился последний клиент. Если
`term` падает на старте (например, не нашёлся по указанному пути, или упал `forkpty`),
сервер тут же закрывается и VT возвращается в текстовый режим.

Чтобы отличить «сломан компоновщик» от «сломан терминал», запустите сервер **без** `-e`:

    sudo ./server.svga            # должен показать рабочий стол/обои и не выходить

Если рабочий стол виден — компоновщик жив, проблема в клиенте: запустите `term` отдельно
(способ 2) и смотрите его сообщения об ошибке на stderr исходной консоли.

Что уже умеет `term` (`$TERM=xterm-256color`): печать с переносом/прокруткой, C0-управление
(CR/LF/BS/TAB/BEL), CSI-перемещения курсора и очистка (`A B C D H f G d J K`), область
прокрутки DECSTBM (`r`) + `IL`/`DL`/`SU`/`SD`, альтернативный экран (DECSET `47`/`1047`/`1049`
— vim/mc не оставляют мусор на экране после выхода), курсорные клавиши в режиме приложения
(DECCKM) и функциональные клавиши F1–F12, переведённые под то, что реально знает
`xterm-256color`-terminfo, цвета SGR — 16-цветные (`0/1/7/30–37/40–47/90–97/100–107/39/49`),
256-цветные и truecolor (`38/48;5;n` и `38/48;2;r;g;b`, хранятся как честный 24-битный RGB на
ячейку), показ/скрытие курсора (DEC `?25`), заголовки окна (OSC `0`/`2`), отчёты мыши (DECSET
`1000`/`1002`/`1006`, см. раздел «Мышь» выше). Достаточно для интерактивного shell и разумно
точного показа изображений через ANSI-арт вьюеры (`chafa`, `catimg`, ...); не полный клон
xterm (нет модификаторов клавиш, bracketed paste и т.п.).

## Примечания

- vt(4) владеет видеорежимом; в userland нет установки режима через VESA. Разрешение
  консоли — то, что задал ядро/KMS (`kern.vt.fb.default_mode`).
- `KD_GRAPHICS` в vt(4) реализован не полностью; на выделенном пустом VT текста просто
  нет, а рендер-цикл исправляет любые повреждения.
- Порядок пикселей предполагается `0x00RRGGBB`. Если цвета выглядят перепутанными, измените
  упаковку в `read_ppm()` (ppm.c).
- Быстрое тестовое изображение:  `printf 'P6\n2 2\n255\n' > test.ppm && \
  printf '\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00' >> test.ppm`

---

## Бэкенды (добавлено): vt_fb / vt_vga / VMware SVGA II

Слой framebuffer теперь имеет три взаимозаменяемых бэкенда за одним и тем же
API `fb.h`. Выбирайте по одному на бинарник; Makefile собирает все три с суффиксами:

| Бэкенд     | Исходник    | Консоль / железо       | Путь к пикселям                         |
|------------|-------------|------------------------|-----------------------------------------|
| `.fb`      | `fb.c`      | vt_fb / scfb / KMS (vtfb0) | `FBIOGTYPE` + `mmap` с tty          |
| `.vga`     | `fb_vga.c`  | vt_vga (vtvga0)        | `/dev/mem` 0xA0000 + `/dev/io` (планарный 12h) |
| `.svga`    | `fb_svga.c` | VMware SVGA II (15ad:0405) | `/dev/pci` + `/dev/io` + `/dev/mem` (линейный + FIFO 2D) |

Какой из них вам нужен:

    sysctl kern.vty                 # должно быть 'vt'
    dmesg | grep -E 'VT\(|vgapci'   # VT(vga)=vtvga0, VT(efifb)/fb=vtfb0
    pciconf -lv | grep -i vmware    # 15ad:0405 => SVGA II присутствует

- **vtfb0** (линейная консоль уже присутствует) -> `.fb`, самый простой, без привилегий
  сверх открытия tty.
- **vtvga0** (`VT(vga)`) -> `.vga`. Замечание: если консоль `VT(vga): text 80x25`,
  карта находится в VGA *текстовом* режиме; `fb_vga.c` предполагает, что она переведена
  в графический режим 12h. На VMware предпочтительнее путь SVGA ниже.
- **VMware SVGA II** -> `.svga`: полноценный линейный 32bpp framebuffer любого разрешения
  плюс аппаратный 2D (`RECT_COPY`/`RECT_FILL`, когда устройство объявляет возможности,
  иначе программный fallback), без X, без DRM/KMS, без vmwgfx.

### Сборка

    make            # все три бэкенда
    make svga       # только бинарники VMware SVGA II
    sudo ./server.svga ./test.ppm

### Требования

Все бэкенды: root, на консоли `vt(4)`. `.vga` и `.svga` дополнительно требуют
`kern.securelevel <= 0` (доступный на запись `/dev/mem`). `.svga` использует `/dev/pci`,
`/dev/io`, `/dev/mem`.

### Переключение VT во время работы сервера

По умолчанию `vtcon` теперь **отказывается** переключать VT, пока владеет консолью
(`lock_switch = 1`): запрос ядра на переключение получает ответ
`VT_RELDISP(VT_FALSE)`, поэтому каждая горячая клавиша переключения (`Alt+Fn`, `Ctrl+Alt+Fn`,
`Alt+стрелки`) и даже сторонний `ioctl(VT_ACTIVATE)` игнорируются, а пользователь
остаётся на нашем VT до выхода сервера (`q` / ESC / SIGINT). Это то, что нужно с
бэкендом SVGA: уход с VT при установленном `SVGA_REG_ENABLE` оставлял устройство
владеющим экраном под другой консолью и вешал/ломал дисплей; отказ от переключения
полностью устраняет это состояние. Поскольку отказ основан на `VT_PROCESS`, смерть
процесса всё равно вызывает сброс ядром на `KD_TEXT` + `VT_AUTO` — в отличие от
`VT_LOCKSWITCH`, падение не может запереть вас на всех VT.

Изменения в `server.c` не требуются; значение по умолчанию просто работает.

Замечание: блокировка переключения находится в процедуре переключения ядра (через
`VT_PROCESS`), а не на конкретной клавише, поэтому не зависит от режима клавиатуры.
Отдельный, более низкоуровневый способ остановить переключение — вывести клавиатуру
из `K_XLATE` в `K_RAW` / `K_CODE` (`ioctl(fd, KDSKBMODE, K_CODE)`): сырые скан-коды
тогда полностью обходят действия переключения из раскладки. Это путь для настоящего
сырого клавиатурного ввода (следующий шаг для этой библиотеки), но осторожно: в
raw/code-режиме tty больше не обрабатывает ввод, поэтому `Ctrl+C` (SIGINT) и
транслированные `q`/`ESC` для выхода перестают работать — вы должны сами декодировать
скан-коды для выхода, иначе можете застрять (переключение тоже заблокировано). Пока
библиотека остаётся в `K_XLATE`, чтобы `q`/`ESC` продолжали работать; отказ через
`VT_PROCESS` уже покрывает каждую горячую клавишу переключения.

Если вы намеренно хотите вернуть кооперативное переключение (разрешить `Ctrl+Alt+Fn`),
вызовите `vtcon_set_switch_lock(&con, 0)` после `vtcon_acquire()`, а затем, на бэкенде
SVGA, возвращайте экран текстовой консоли при переходах `con.active`:

    #include "fbsvga.h"
    vtcon_set_switch_lock(&con, 0);        /* снова включить кооперативное переключение */
    /* ... внутри рендер-цикла, после vtcon_pump(&con): */
    static int was_active = 1;
    if (con.active != was_active) {
        fb_svga_enable(fb, con.active);    /* 0 -> текстовый vt; 1 -> наш кадр */
        was_active = con.active;
    }

`fb_close()` уже сбрасывает `SVGA_REG_ENABLE`, поэтому чистый выход восстанавливает
текстовую консоль в любом случае.

### SVGA II: порядок запуска (рекомендуемый первый тест)

1. `fb_svga_open(1024,768,32,0)`, затем `fb_svga_fill(fb,0,0,1024,768,0x0000FF)`
   -> сплошной синий экран проверяет обнаружение PCI, порты, оба mmap, установку
   режима и FIFO+UPDATE за один раз.
2. `ppm2fb.svga image.ppm` (одинарная/двойная буферизация).
3. `server.svga` с патчем VT-кооперации выше.

### Константы для проверки по `svga_reg.h` от VMware

`fb_svga.c` помечает пометкой `[VERIFY]` номера легаси-команд 2D
(`SVGA_CMD_RECT_FILL`/`RECT_COPY`) и биты возможностей (`SVGA_CAP_RECT_FILL`/`COPY`).
Ошибка в них лишь отключает аппаратный 2D (есть программный fallback); основной путь
(рукопожатие ID -> WIDTH/HEIGHT/BPP -> ENABLE -> рисование в VRAM -> UPDATE)
использует только стабильные, общеизвестные значения регистров.

---
---

# English

# libfb-vt

Console framebuffer graphics and a display-server seed for FreeBSD 10+ (vt(4)).
Pure native FreeBSD interfaces — no `/dev/mem`, no kvm, no DRM/KMS, no linuxkpi.
Only `<sys/fbio.h>`, `<sys/consio.h>`, `<sys/kbio.h>`.

## Layers

- `fb.c` / `fb.h`     — framebuffer: open + `mmap` via `FBIOGTYPE`, software double buffer.
- `ppm.c` / `ppm.h`   — PPM loader (P6/P3, comments, any maxval) + 32bpp blit/cls.
- `vtcon.c` / `vtcon.h` — native VT/console ownership: `VT_OPENQRY` grab, `KD_GRAPHICS`,
  and the `VT_SETMODE(VT_PROCESS)` switch handshake (the mechanism X uses).
- `mouse.c` / `mouse.h` — mouse pointer: reads `/dev/sysmouse` (needs `moused(8)` running),
  decodes the sysmouse protocol (level 1, 8-byte packets, falls back to level 0) into
  relative dx/dy/wheel/buttons and an absolute, screen-clipped cursor position.

## Programs

- `ppm2fb image.ppm`        — one-shot: draw an image once on the current console.
- `fbshow  image.ppm [vt]`  — persistent viewer (polling; grabs a free VT).
- `server  image.ppm [vt]`  — display-server seed: exclusive VT ownership + render loop.
- `cube    [vt]`            — demo: a spinning solid-shaded 3D cube in a 320x200 window
  (same server skeleton; software renderer, works on every backend). Needs `-lm`.

`[vt]` is a 1-based VT number (7 == ttyv6); omit or 0 to auto-pick the first free VT.

## Build

    make
    # or without make:
    cc -Wall -O2 -o server server.c vtcon.c ppm.c fb.c

## Run

Requires root, on a vt(4) console (`sysctl kern.vty` must report `vt`).

    sudo ./server ./test.ppm

Quit with `q` / ESC (or SIGINT/SIGTERM). `server` refuses VT switching
(`Alt+Fn`, `Ctrl+Alt+Fn`, `Alt+arrows`) while it runs; see the section below.

## Notes

- vt(4) owns the video mode; there is no userland VESA mode-setting. The console
  resolution is whatever the kernel/KMS set (`kern.vt.fb.default_mode`).
- vt(4)'s `KD_GRAPHICS` is incomplete; on a dedicated empty VT there is simply no
  text to draw, and the render loop repairs any damage.
- Pixel order assumed `0x00RRGGBB`. If colours look swapped, change the packing
  in `read_ppm()` (ppm.c).
- Quick test image:  `printf 'P6\n2 2\n255\n' > test.ppm && \
  printf '\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00' >> test.ppm`

---

## Backends (added): vt_fb / vt_vga / VMware SVGA II

The framebuffer layer now has three interchangeable backends behind the same
`fb.h` API. Pick one per binary; the Makefile builds all three, suffixed:

| Backend    | Source      | Console / HW           | Path to pixels                          |
|------------|-------------|------------------------|-----------------------------------------|
| `.fb`      | `fb.c`      | vt_fb / scfb / KMS (vtfb0) | `FBIOGTYPE` + `mmap` off the tty     |
| `.vga`     | `fb_vga.c`  | vt_vga (vtvga0)        | `/dev/mem` 0xA0000 + `/dev/io` (planar 12h) |
| `.svga`    | `fb_svga.c` | VMware SVGA II (15ad:0405) | `/dev/pci` + `/dev/io` + `/dev/mem` (linear + FIFO 2D) |

Which one you need:

    sysctl kern.vty                 # must be 'vt'
    dmesg | grep -E 'VT\(|vgapci'   # VT(vga)=vtvga0, VT(efifb)/fb=vtfb0
    pciconf -lv | grep -i vmware    # 15ad:0405 => SVGA II present

- **vtfb0** (linear console already present) -> `.fb`, simplest, no privileges
  beyond opening the tty.
- **vtvga0** (`VT(vga)`) -> `.vga`. Note: if the console is `VT(vga): text 80x25`
  the card is in VGA *text* mode; `fb_vga.c` assumes it has been put into 12h
  graphics. On VMware, prefer the SVGA path below instead.
- **VMware SVGA II** -> `.svga`: full linear 32bpp framebuffer at any resolution
  plus hardware 2D (`RECT_COPY`/`RECT_FILL` when the device advertises the caps,
  software fallback otherwise), with no X, no DRM/KMS, no vmwgfx.

### Build

    make            # all three backends
    make svga       # just the VMware SVGA II binaries
    sudo ./server.svga ./test.ppm

### Requirements

All backends: root, on a `vt(4)` console. `.vga` and `.svga` additionally need
`kern.securelevel <= 0` (writable `/dev/mem`). `.svga` uses `/dev/pci`, `/dev/io`,
`/dev/mem`.

### VT switching while the server runs

By default `vtcon` now **refuses** VT switches while it owns the console
(`lock_switch = 1`): the kernel's switch request is answered with
`VT_RELDISP(VT_FALSE)`, so every switch hotkey (`Alt+Fn`, `Ctrl+Alt+Fn`,
`Alt+arrows`) and even a foreign `ioctl(VT_ACTIVATE)` is ignored, and the user
stays on our
VT until the server quits (`q` / ESC / SIGINT). This is what you want with the
SVGA backend: leaving the VT while `SVGA_REG_ENABLE` is set left the device
owning the screen under another console and crashed/wedged the display; refusing
the switch removes that state entirely. Because the refusal rides on
`VT_PROCESS`, process death still triggers the kernel's `KD_TEXT` + `VT_AUTO`
reset -- unlike `VT_LOCKSWITCH`, a crash can't lock you out of every VT.

No `server.c` change is required; the default just works.

Note: the switch block is at the kernel switch routine (via `VT_PROCESS`), not at
a specific key, so it does not depend on keyboard mode. A separate, lower-level
way to stop switching is to take the keyboard out of `K_XLATE` into `K_RAW` /
`K_CODE` (`ioctl(fd, KDSKBMODE, K_CODE)`): raw scancodes then bypass the keymap's
switch actions entirely. That is the path for real raw keyboard input (the next
step for this library), but beware: in raw/code mode the tty no longer cooks
input, so `Ctrl+C` (SIGINT) and the translated `q`/`ESC` quit stop working -- you
must decode scancodes yourself for quit, or you can get stuck (switching is also
blocked). For now the library stays in `K_XLATE` so `q`/`ESC` keep working; the
`VT_PROCESS` deny already covers every switch hotkey.

If you deliberately want cooperative switching back (allow `Ctrl+Alt+Fn`), call
`vtcon_set_switch_lock(&con, 0)` after `vtcon_acquire()`, and then, on the SVGA
backend, hand the screen back to the text console on `con.active` transitions:

    #include "fbsvga.h"
    vtcon_set_switch_lock(&con, 0);        /* re-enable cooperative switching */
    /* ... inside the render loop, after vtcon_pump(&con): */
    static int was_active = 1;
    if (con.active != was_active) {
        fb_svga_enable(fb, con.active);    /* 0 -> vt text; 1 -> our frame */
        was_active = con.active;
    }

`fb_close()` already drops `SVGA_REG_ENABLE`, so a clean exit restores the text
console regardless.

### SVGA II: bring-up order (recommended first test)

1. `fb_svga_open(1024,768,32,0)` then `fb_svga_fill(fb,0,0,1024,768,0x0000FF)`
   -> a solid blue screen validates PCI discovery, ports, both mmaps, mode set
   and FIFO+UPDATE in one shot.
2. `ppm2fb.svga image.ppm` (single/double buffered).
3. `server.svga` with the VT-cooperation patch above.

### Constants to verify against VMware's `svga_reg.h`

`fb_svga.c` marks with `[VERIFY]` the legacy 2D command numbers
(`SVGA_CMD_RECT_FILL`/`RECT_COPY`) and cap bits (`SVGA_CAP_RECT_FILL`/`COPY`).
Getting these wrong only disables hardware 2D (there is a software fallback); the
core path (ID handshake -> WIDTH/HEIGHT/BPP -> ENABLE -> draw to VRAM -> UPDATE)
uses only stable, well-known register values.
