# Архитектура: слои

- `fb.c` / `fb.h` — framebuffer: открытие + `mmap` через `FBIOGTYPE`, программная двойная буферизация.
- `ppm.c` / `ppm.h` — загрузчик PPM (P6/P3, комментарии, любой maxval) + 32bpp blit/cls.
- `vtcon.c` / `vtcon.h` — нативное владение VT/консолью: захват через `VT_OPENQRY`, `KD_GRAPHICS`
  и рукопожатие переключения `VT_SETMODE(VT_PROCESS)` (тот же механизм, что использует X).
- `mouse.c` / `mouse.h` — курсор мыши: читает `/dev/sysmouse` (нужен запущенный `moused(8)`),
  декодирует относительные dx/dy/колесо/кнопки в протоколе sysmouse (уровень 1, 8 байт, с
  откатом на уровень 0), копит абсолютную позицию с клипом по экрану.
- `kbd.c` / `kbd.h` — декодер сырых скан-кодов клавиатуры (`K_CODE`, AT/PS2 set 1) с
  реальным состоянием Shift/Ctrl/Alt — опционально (`server -k`), нужен только для комбинаций
  вроде Shift+стрелка, которые `K_XLATE` не может выразить (см. [input.md](input.md)).
- `proto.c` / `proto.h` — протокол display-сервера: кадрированные сообщения поверх
  `AF_UNIX`-сокета + передача fd через `SCM_RIGHTS` (для разделяемого буфера пикселей).
- `fontspleen.h` — встроенный моноширинный шрифт 8×16 (Spleen, BSD-2-Clause), проиндексированный
  по Unicode: ASCII + Latin-1 + кириллица + псевдографика + блочные символы. Поиск глифа — `glyph_for(cp)`.
- `mgl.c` / `mgl.h` — программный (software) OpenGL-подобный слой. См. [programs.md](programs.md#mgl).

Подробности по бэкендам framebuffer (`fb.c`/`fb_vga.c`/`fb_svga.c`) — [backends.md](backends.md).
Подробности по вводу (мышь/клавиатура) — [input.md](input.md).
Список программ и что умеет `term` — [programs.md](programs.md).
