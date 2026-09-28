# TODO

## Przed scaleniem `shared-tui-lib` z `master`

- [ ] Przetestować 7afilm-tui: UTF-8 w polach tekstowych (Cfg → Name), wspólny
      pasek zakładek, popupy bez cienia, Ctrl+Q, usuwanie presetów (Delete
      w wyszukiwarce „Load Preset”)
- [ ] Przetestować 7aorganizer-tui (`./7aorganizer-tui --import poc.db`, potem
      normalne uruchomienie)
- [ ] Zbudować i uruchomić oba programy na OpenBSD, także na konsoli wscons
      (`-lcurses`, `get_wch`, `NCURSES_WIDECHAR`)

## 7aorganizer-tui

- [ ] Wielowierszowy opis: formularz edycji ma jedno pole tekstowe, więc przy
      edycji opisu nowe linie zamieniają się na spacje
- [ ] Wpisy cykliczne: usunięcie kasuje całą serię; brak pominięcia jednego
      wystąpienia (np. odwołany standup) ani edycji tylko jednego terminu
- [ ] Wyszukiwanie `/` także w opisach, nie tylko w tytułach i datach
- [ ] Cofnięcie odhaczenia: zrobione zadanie od razu znika z dashboardu,
      przywrócić je można tylko w F3 z filtrem „done”
- [ ] Opcjonalnie: obsługa myszy (klikanie w przyciski i elementy listy)
- [ ] Opcjonalnie: zakładki Help (F11) i Quit (F12) jak w 7afilm

## 7afilm-tui

- [x] Stare ostrzeżenia kompilatora: wcięcia w `film/timer.c` (ParseCountdownFields)
      i przycinanie nazwy presetu w `BuildPresetName` (`-Wformat-truncation`)
- [x] Migracje bazy przez `db_migrate()` (`PRAGMA user_version`) zamiast
      sprawdzania kolumn (`db_column_exists("presets", "dev_hh")`)

## Repozytorium

- [ ] Przemianować repo i katalog (np. `7a-tui`), bo mieści dwie aplikacje
- [ ] `todo-organizer.md`: zaktualizować do tego, co powstało (schemat bazy,
      zakładki F1–F3, klawisze, formularz, quick add `a`) albo usunąć
- [ ] `poc.db` w katalogu głównym: kopia jest w `organizer/testdata/`, oryginał
      można usunąć
