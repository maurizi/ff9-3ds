# Dual-screen UI design

The PS1 game has one screen; the 3DS has a 400×240 stereoscopic top screen and a 320×240 touch screen below it, and
all of the PS1's buttons. The redesign follows what the best-received DS/3DS RPGs did (Dragon Quest VII/VIII 3DS,
Bravely Default, Chrono Trigger DS, Shin Megami Tensei IV, Fire Emblem Awakening):

- **The top screen is the world; the bottom screen is information and decisions.** Nothing is shown on both.
- **No virtual controls.** The buttons do everything; touch is an optional shortcut onto the same choices (the game's
  own cursor logic), with large targets at fixed places (≥ 40×32 px).
- **Nothing on the bottom screen is needed in the moment of action.** Speech, "!" prompts, damage numbers, ability
  names and the target cursor stay on top, where the eyes are.
- **Top-screen 2D UI sits at the screen plane** in stereo; the scene is behind it.

## Per mode

| mode | top screen | bottom screen |
|---|---|---|
| field | the field; speech bubbles and prompts with their speakers **(done)** | area map (walkmesh, Zidane, exits), location name, party HP/MP strip, buttons: Menu, ATE (lit when one is available), Cards, Mognet at a moogle |
| dialogue choices | the question | the choices as large buttons (cursor still works) |
| battle | the scene alone: damage, ability banner, target hand | commands (left) and four party cards with HP/MP/ATB/Trance (right); targets as buttons laid out like the formation |
| main menu | detail for the sub-menu: model and stat comparison (equip), AP progress (abilities), stat sheet (status), item text | the PS1 menu at 1:1, touchable |
| shop / synthesis | who can equip it and the stat change, abilities taught | item list, quantity, buy/sell |
| save | the selected slot's details | slots |
| world map | the world, without the PS1 minimap | a large world map: position, places found |
| Tetra Master | the highlighted card large, score, flips in stereo | board and hands, tap/drag to place |
| Mognet | the open letter | letters, moogles |
| FMV | the movie | static |

## Status
- Done: field and world-map windows stay on the top screen; the virtual pad and the copy of the scene are gone.
- Done: the field bottom screen, drawn by the game's own window/text code (port/game/field_hud.c): location,
  party HP/MP, time and gil, a MENU button (tap = triangle).
- Done: battle target hand on the top screen; the game's target window (ENEMY / PLAYER) is the bottom screen's
  touchable target panel (port/game/battle_ui.c).
- Done: world map: the full navigation map always on the bottom screen, no minimap on top (port/game/world_navi.c).
- Done: Tetra Master: the board on the bottom screen; the top magnifies (2x) where the game's cursor points.
- Next: menus with their top-screen detail views, shops; the field's area map (walkmesh, exits).
