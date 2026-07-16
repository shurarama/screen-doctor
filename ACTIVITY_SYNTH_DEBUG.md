# Активность (XI_RawMotion) не доставляется в реальный Time Doctor

Рабочий журнал по проблеме синтеза input-activity в `grab_override.so`.
Ведётся хронологически: симптом → гипотезы → правки → проверка.

## Симптом

В реальном Time Doctor синтетическая активность (XI_RawMotion) доставляется
**только один раз при старте**, а дальше — тишина. Time Doctor считает
пользователя простаивающим при работе в нативных Wayland-приложениях, чей ввод
не доходит до XWayland.

Признак в логе `/tmp/grab_override.log` (pid реального TD):

```
diag:activity monitor display=... fd=35 noted (raw-motion subscription)
diag:activity state fresh=1 (in-process wayland watcher)
diag:activity poll fd=35 real_ready=0 fresh=1 faked=1
diag:activity synth evtype=RawMotion device=2 cookie=0x5d000000
   <-- и больше НИ ОДНОГО diag:activity synth / poll / epoll
```

Свежесть (`fresh=1`) в порядке, механизм инъекции работает (один synth прошёл),
но повторной доставки нет.

## Как устроен мост активности

- `screen-doctor-activity-helper` / in-process wayland-watcher следит за
  `ext_idle_notifier_v1` и пишет «пользователь активен» в разделяемый файл
  (`activity_state.h`).
- `grab_override.so` внутри TD читает файл; пока свежо — фабрикует
  `XI_RawMotion`, чтобы XInput2-idle-монитор TD видел активность.
- Синтетическое событие «вооружается» флагом `activity_synth_pending`, затем
  хуки `XPending`/`XEventsQueued`/`XNextEvent`/`XGetEventData` отдают TD один
  поддельный `RawMotion` (cookie с сентинелом `0x5D000000`).

Ключевой вопрос — **где TD ждёт события на монитор-соединении** (fd 35), потому
что именно там нужно выставлять `activity_synth_pending`.

## Хронология

### Attempt 1 — epoll-реконсиляция (НЕ сработало для этого TD)

**Гипотеза:** TD (Chromium/Qt) регистрирует fd 35 в epoll на этапе открытия
Display — до `XISelectEvents`, поэтому хук `epoll_ctl` промахивался по охранке
`fd == monitor_fd` (monitor_fd тогда ещё −1), и `activity_monitor_epoll_data`
никогда не выставлялся → `epoll_wait` не инжектил.

**Правка:**
- Кольцевая таблица `activity_epoll_table[256]` — запоминает все
  `EPOLL_CTL_ADD/MOD (epfd, fd, data)` независимо от того, известен ли монитор-fd.
- В `activity_note_raw_subscription` — `activity_epoll_reconcile_locked(fd)`:
  задним числом усыновляет регистрацию из таблицы.
- `epoll_ctl` пишет все ADD/MOD в таблицу, DEL — удаляет и сбрасывает `..._valid`.

**Результат:** в логе `reconciled from table = 0`, `epoll = 0`. Не сработало.

**Почему:** диагностикой доказано, что TD **вообще не использует epoll для fd 35**.

Правка оставлена в коде: она корректна и покрывает epoll-дренаж в других
сборках TD, вреда не наносит. Но не была причиной для этой версии.

### Диагностика (ground truth)

`strace` в системе нет; ptrace_scope=0. Считали состояние из `/proc`:

```
# треды и системный вызов, в котором висят
for t in /proc/<PID>/task/*; do
  printf "%s %s wchan=%s syscall=%s\n" "${t##*/}" \
    "$(cat $t/comm)" "$(cat $t/wchan)" "$(cut -d' ' -f1 $t/syscall)"
done

# декодировали pollfd-массивы из /proc/<PID>/mem по адресу из /proc/<tid>/syscall
# (poll=7, ppoll=271, epoll_wait=232), искали fd 35
```

**Находки:**
- fd 35 (`socket:[...]`) — отдельное X-соединение монитора TD (не главный Qt
  X-конн, тот — fd 24, его читает тред `QXcbEventQueue`).
- **fd 35 не присутствует ни в одном pollset** — 0 из 40 замеров за 2 сек.
- **fd 35 не состоит ни в одном epoll-инстансе** (проверка по `tfd:` в
  `/proc/<PID>/fdinfo/<epfd>`).
- При этом реальные события клавиш с fd 35 читаются (в логе
  `diag:xinput XNextEvent ... RawKeyPress`).

**Вывод:** TD дренит монитор-соединение **по таймеру** — периодически зовёт
`XPending`/`XNextEvent`, ни на чём не блокируясь. Поэтому ни poll-, ни
epoll-adjust для fd 35 никогда не вызывается, `activity_synth_pending` не
выставляется, synth не доставляется. Единственный synth на старте пришёл из
разовой `poll()` внутри setup `XISelectEvents`/`XSync`.

### Attempt 2 — вооружение synth прямо в XPending/XEventsQueued (текущее)

**Правка** (`grab_override.c`, хуки `XPending`/`XEventsQueued`):

```c
static int activity_report_synth_for_drain(Display *display) {
    if (!activity_enabled() || !activity_display_is_monitor(display)) return 0;
    if (activity_synth_pending_get()) return 1;
    return activity_should_fake_wakeup(); /* rate-gated; выставляет pending */
}
```

Когда реальная очередь на монитор-дисплее пуста и пользователь активен —
запускаем rate-gated wakeup прямо в точке таймерного дренажа, так что
следующий `XNextEvent` TD получит синтетический `RawMotion`. Rate-gate
(`activity_rate_ms`) не даёт флудить: не чаще одного события за интервал.

**Статус:** ✅ ПОДТВЕРЖДЕНО. После перезапуска TD (pid 481944, сборка 20:26)
в логе пошёл непрерывный поток synth — по одному в секунду, с растущим cookie:

```
20:26:49 diag:activity synth evtype=RawMotion device=2 cookie=0x5d000007
20:26:50 diag:activity synth evtype=RawMotion device=2 cookie=0x5d000008
...
20:27:13 diag:activity synth evtype=RawMotion device=2 cookie=0x5d00001f
```

`reconciled from table = 0`, `epoll registered = 0` — доставку обеспечил именно
путь `XPending`/`XEventsQueued`, что подтверждает диагноз таймерного дренажа.

## Как проверить после перезапуска TD

1. Перезапустить Time Doctor (главный процесс должен подгрузить свежую `.so`;
   проверить `grep grab_override.so /proc/<pid>/maps` и время сборки `.so`).
2. В `/tmp/grab_override.log` по pid реального TD должны пойти **периодические**
   (не единичные) строки:
   ```
   diag:activity synth evtype=RawMotion device=2 cookie=0x5d0000NN
   ```
   с растущим счётчиком cookie, примерно раз в `activity_rate_ms`.
3. В самом Time Doctor активность при работе в нативных Wayland-окнах должна
   перестать сбрасываться в idle.

## Полезные команды диагностики

```bash
# кто сейчас держит библиотеку
for d in /proc/[0-9]*; do grep -qs grab_override.so "$d/maps" 2>/dev/null \
  && echo "$(basename $d) $(tr '\0' ' ' <$d/cmdline)"; done

# в каком системном вызове висят треды TD
for t in /proc/<PID>/task/*; do echo "${t##*/} $(cat $t/comm) $(cat $t/wchan)"; done

# есть ли fd 35 в каком-либо epoll
for e in /proc/<PID>/fd/*; do [ "$(readlink $e)" = "anon_inode:[eventpoll]" ] \
  && grep -l '^tfd:.*  *35\b' /proc/<PID>/fdinfo/${e##*/}; done
```
