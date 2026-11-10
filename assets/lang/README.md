# Recharged's texts

These files hold the texts of Recharged's own menus: the launcher, the game
menu (F1 or HOME), Recharged's pages in the game and its messages. The game's
own texts (its menus, its commentary) come from the game disc in the game's
language and are not here.

Each language has a folder named by its language code (`de`, `fr`, `pt-BR`,
`zh-Hans`), with the same files as `en`:

- `common.json`: texts the launcher and the game share (gameplay options,
  sound groups, keys), and the language's own name for itself
  (`language.name`, shown in the language setting);
- `launcher.json`: the launcher's Play and Game pages, its updates and
  dialogs, and the names of captains, sidekicks, stadiums and items;
- `match_setup.json`: the online profile, Miis, sidekicks, match rules and
  presets;
- `settings.json`: the launcher's Display, Audio, Controls, Gameplay,
  Advanced and About pages;
- `menus.json`: Recharged's menus and messages in the game.

The files are JSON in i18next's v4 layout, which translation tools read
(Weblate, Crowdin, Poedit, a text editor).

## Translating

1. Copy the `en` folder and name the copy after your language's code.
2. Translate the texts, never the keys (the names before the colons). Leave
   out or empty what you haven't translated yet: the English text shows
   there.
3. Keep every `{{placeholder}}` as it is (a player's name, a number), placed
   where your language wants it.
4. Texts that count something come in forms: `_one` and `_other` in English.
   Give your language's forms (`_one`, `_few`, `_many`, `_other` and so on,
   by the [CLDR plural rules](https://www.unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html)).
5. Texts in capitals in English (titles, buttons) are capitals in the game's
   look: write them in capitals too.

Recharged shows a new folder at its next start. It follows the PC's
language by default; the launcher's Game page sets another
(`[interface] language` in `recharged.ini`).

Fonts: Recharged's own font has Latin, Greek and Cyrillic letters. Other
scripts (Japanese, Chinese, Korean, Arabic, ...) come from the fonts the PC
has.
