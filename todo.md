# TODO - 7afilm-tui dropdowns

## Do zrobienia

- [ ] Dodac `FT_DROPDOWN = 6`, `MAX_DROPDOWNS = 8` do stalych
- [ ] Dodac tablice opcji: `film_options[]`, `iso_options[]`, `dev_options[]`
- [ ] Dodac struct `DropdownMeta`
- [ ] Dodac `iso_used_buf[8]` do `CountdownTimer`
- [ ] Dodac globalne indeksy (`g_film_idx`, `g_iso_nom_idx`, `g_iso_used_idx`, `g_dev_idx`) i `g_dd_store[]`
- [ ] Nowe funkcje pomocnicze: `find_option_idx`, `sync_dropdown_indices`, `sync_bufs_from_indices`, `AutoUpdatePresetName`
- [ ] `fields_reset()` — dodac `g_dd_count = 0`
- [ ] Nowa funkcja `draw_dropdown()`
- [ ] `draw_timer_fields()` — zamienic Film/ISO/Developer na dropdowny, dodac wiersz "ISO used"
- [ ] `draw_presets()` — usunac pole nazwy i przycisk Save
- [ ] Nowa funkcja `draw_save_section()` (nazwa auto + przycisk Save na dole)
- [ ] `draw_database_tab()` — wywolac `draw_save_section()` na koncu
- [ ] `handle_key()` — obsluga `FT_DROPDOWN` (Up/Down cykluje opcje)
- [ ] `OpenDatabase()` — dodac kolumne `dev_iso_used`
- [ ] `LoadPresetIntoTimers()` — dodac `dev_iso_used`, wywolac `sync_dropdown_indices()`
- [ ] `SavePreset()` — dodac `dev_iso_used` do zapytania
- [ ] `main()` — wywolac `sync_bufs_from_indices()` po `InitTimers()`
- [ ] Zaktualizowac pasek pomocy
- [ ] `make` — kompilacja bez bledow
