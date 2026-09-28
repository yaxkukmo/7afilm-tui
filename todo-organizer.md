# 7aorganizer-tui

Osobisty organizer w terminalu: zadania (todo) i wpisy w kalendarzu w jednej
aplikacji, obsługiwany z klawiatury.  Baza: `~/.7a/organizer.db`.

```
7aorganizer-tui                  uruchomienie
7aorganizer-tui --import FILE    import z poc.db albo tasks.db z 7atodo/7acal (pusta baza docelowa)
```

Pełny opis klawiszy jest w nagłówku `organizer/7aorganizer-tui.c`, a pozostała
praca w `todo.md` (sekcja 7aorganizer-tui).

## Układ

```
 F1:Dashboard  F2:Calendar  F3:Todo
┌─ lista ──────────────────────┬─ podgląd ────────────────┐
│ TODAY                        │ Team meeting             │
│ ▶ 10:00 Team meeting         │ 30.09.2026 10:00, 60 min │
│ TODO                         │                          │
│   Napisać raport             │ opis ...                 │
│ TOMORROW                     │                          │
│   09:00 Standup              │                          │
│ THIS WEEK                    │                          │
└──────────────────────────────┴──────────────────────────┘
 n:New  a:Quick add  Enter:Edit  Space:Done  s:Schedule  x:Delete  /:Search  q:Quit
```

Dolny pasek pokazuje przyciski pasujące do zaznaczenia i zakładki.

Lista i podgląd dzielą ekran po połowie; na terminalu węższym niż 50 kolumn
widać tylko listę.  Popupy nie mają cienia.

## Zakładki

- **F1 Dashboard** (ekran główny): dziś, otwarte zadania, jutro, reszta
  tygodnia (dziś + 6 dni); Tab / S-Tab skacze między sekcjami.  Zadania
  odhaczone dziś zostają na końcu sekcji TODO z `[x]`, więc Space może je
  przywrócić; następnego dnia znikają
- **F2 Calendar**: miesiąc jak w cal(1); strzałki zmieniają dzień (góra/dół
  o tydzień), PgUp/PgDn miesiąc, Home dziś, Tab / j / k wybiera wpis dnia
- **F3 Todo**: płaska lista zadań, `f` przełącza filtr open → done → all

## Klawisze

| Klawisz          | Akcja                                                 |
|------------------|-------------------------------------------------------|
| `↑↓` `j` `k`     | ruch po liście; PgUp/PgDn, Home/End                   |
| `Enter`, `e`     | edycja zaznaczonego wpisu lub zadania                 |
| `Space`          | odhaczenie zadania albo jego przywrócenie             |
| `s`              | zaplanowanie zadania: formularz wpisu powiązanego z nim |
| `x`              | usunięcie (potwierdzenie y/n)                         |
| `n`              | nowy wpis lub zadanie w formularzu                    |
| `a`              | quick add w jednej linii                              |
| `/`              | wyszukiwanie po tytule, dacie i opisie, skok do wyniku |
| `[` `]`          | przewijanie długiego opisu w podglądzie (też S-↑/S-↓) |
| `F1` `F2` `F3`   | zakładki                                              |
| `q`, `Ctrl+Q`    | wyjście                                               |

## Formularz edycji

Wyśrodkowany panel (`organizer/edit.c`), Tab / strzałki między polami, Enter
lub Save (albo Ctrl+S) zapisuje, Esc / Cancel zamyka.

- Title, Type (tylko nowy element: calendar entry / todo)
- wpis: Repeat (none / daily / weekly / monthly / yearly), zależnie od niego
  Date („30.09, jutro, pt, +3d”), Weekday albo Day (i Month), dalej Time
  i Duration w minutach
- zadanie: Priority (high / normal / low), przy edycji Status (open / done)
- Notes: opis; jednowierszowy można pisać w polu, a Ctrl+E otwiera go
  w `$VISUAL`, `$EDITOR`, a bez nich w `nvim` (albo `vi`).  Opis
  z kilkoma liniami zmienia się tylko w edytorze: pole pokazuje pierwszą
  linię i „(+N lines)”, Enter na nim otwiera edytor.  Wyjście z błędem
  (`:cq`) zostawia opis bez zmian

Nowy wpis zaczyna się od dnia zaznaczonego w kalendarzu albo od dziś.
Edycja wpisu cyklicznego zmienia całą serię.

## Quick add (`a`)

`organizer/quickadd.c`, słowa rozpoznawane w dowolnym miejscu, po polsku lub
angielsku:

- data: `dziś` `jutro` `pojutrze` / `today` `tomorrow`, dzień tygodnia
  (`pn`, `piątek`, `mon` ...: najbliższy po dziś), `+3`, `+3d`, `+2w`,
  `30.09`, `30.09.2026`, `2026-09-30`
- godzina: `15:00`, `9:30`, `3pm`, `10:30am` (sama godzina = dziś)
- priorytet: `!high` `!h` `!1` `!wysoki`, `!normal` `!2`, `!low` `!l` `!3` `!niski`
- reszta to tytuł; `w`, `we`, `o`, `at`, `on` przed datą lub godziną znikają

Z datą lub godziną powstaje wpis w kalendarzu, bez nich zadanie:
`dentysta w piątek o 15:00` → wpis, `kupić mleko !h` → zadanie.

## Baza

Schemat w `organizer/store.c`, wersjonowany przez `db_migrate()`
(`PRAGMA user_version`); kroki migracji tylko się dopisuje.  Tabele mają
układ z poc.db, żeby import szedł bez przeróbek.

- `todos`: title, description, priority 1–3, status open/done,
  created_at, updated_at, done_at (kiedy odhaczone; migracja 2)
- `calendar_entries`: title, description, albo `entry_date`, albo
  `recurrence_type` (daily / weekly / monthly / yearly) z
  `recurrence_weekday` / `recurrence_day` / `recurrence_month`;
  `entry_time`, `duration_min`, `todo_id` (zadanie, z którego wpis
  zaplanowano; usunięcie zadania zostawia wpis bez powiązania)
- synchronizacja (migracja 3): obie tabele mają `uuid` i `updated_at`,
  a usunięte elementy zostawiają `uuid` w `deleted_items`.  Pilnują tego
  triggery w bazie, więc tak samo zachowują się zapisy z 7atodo i 7acal
  (repo 7adesktop), które korzystają z tej bazy.  `7async` wysyła zmiany
  na serwer i pobiera cudze.

Z `tasks.db` (7atodo/7acal): wpis z datą → wpis w kalendarzu (z godziną),
bez daty → zadanie; pierwsza linia treści to tytuł, reszta opis; `uuid`
zostaje ten sam, więc import na drugiej maszynie nie dubluje rekordów.

Testy warstwy danych: `make check` (`organizer/test_store.c`, dane
w `organizer/testdata/poc.db`).
