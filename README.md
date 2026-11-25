# Grandia III: Open Skies

*A native PC & Android port of Grandia III.*

**Grandia III** (PlayStation 2, 2005) as a real PC game: sharper, wider and smoother than on the
console, with savestates, subtitles and a German and French translation - and, if you like, as an
undub with the original Japanese voices.

This is not an emulator. The game's program was translated into PC code once, ahead of time, so the game
runs on your PC like any other PC game.

**No game data is included.** You need your own copy of *Grandia III (USA)*. The program reads the disc
images (ISO files) of your discs while you play; it never changes or copies them.

This is a fan project, not affiliated with Square Enix or Game Arts.

## Download and play

1. Download `Grandia3-PC-v*.zip` from the [Releases](../../releases) page and unzip it into a folder of
   its own.
2. Start `Grandia3.exe` and choose the image of your disc 1 (for example
   `Grandia III (USA) (Disc 1).iso`).

Tip: put the image next to `Grandia3.exe` as `disc1.iso` (disc 2: `disc2.iso`) and the game finds it
by itself - no file dialog, handy on the Steam Deck.

**Requirements:** Windows 10 or 11 (64 bit) with a graphics card for Direct3D 11.3, or a Steam Deck /
Linux with Proton.

Updating: unzip the new version over the old folder. Settings, saves and savestates stay.

Everything about keys, settings and options: [docs/README.txt](docs/README.txt)
(German: [docs/LIESMICH.txt](docs/LIESMICH.txt)).

## What you get

**Picture**
- Up to 8 times the original resolution, chosen automatically for your screen
- Real widescreen (16:9, 16:10, or ultrawide 21:9 and 32:9), with menus and text kept in shape
- 60 fps in the field and, if you want, in battles - at the correct game speed; cutscenes run at
  30 fps like the original, so they stay in sync with the voices
- Sharp text, sharpening and image filters
- Optional HD texture pack (see below)

**Comfort**
- Savestates anywhere, also in battles and cutscenes: 9 slots, plus an automatic one after every map
  change
- Fast forward (hold Tab, or Back + R2 on the controller)
- Skip cutscenes and movies with START
- Turn off battles on monster contact to explore in peace (story and boss battles still happen)
- Large dialogue windows for small screens (automatic on the Steam Deck)
- An in-game settings menu (F1, or Back + Start / the PS button), its size adjustable

**Sound and language**
- The original English text, or complete German and French fan translations
- Subtitles for all voiced cutscenes and movies, in English, German or French (these scenes have no
  text in the original)
- **Undub:** with an image of your own *Grandia III (Japan)* disc 1, cutscenes, battle shouts and
  movies play with the original Japanese voices, while text and subtitles stay in your language
- Separate volume for music, voices, effects and movies

**Controls**
- Xbox and other XInput controllers, and the DualShock 4 and DualSense (USB or Bluetooth) directly,
  with rumble - Steam Input and DS4Windows keep working too
- Keyboard keys can be changed in the settings

**Steam Deck**
- Recognised automatically: 16:10 widescreen, full screen, large dialogue windows and larger menus
- Shortcuts on the controller: View + Menu = settings, View + R1 / L1 = save / load a savestate,
  View + R2 = fast forward
- Add `Grandia3.exe` to Steam as a non-Steam game and choose Proton in its compatibility settings

## Undub (Japanese voices)

Put the image of your Japanese disc 1 next to the game or next to your US image, named
`disc1_jp.iso` or `Grandia III (Japan) (Disc 1).iso`. Then switch on *Undub* in the F1 menu and
restart the game. If the image is not found, the game asks for it once. The Japanese disc covers
disc 2 as well; only the disc 2 movies keep their English sound.

## HD texture pack (optional)

An unofficial pack with all 7,604 textures of the game upscaled 4 times (7.5 GB, separate download):
[part 1](https://mega.nz/file/HsAA1ZJI#XaEf7ifCbabg8oY0FB5-TBd1A7-65ooXDTEMhyZIy5U) and
[part 2](https://mega.nz/file/C9JmECLB#aYbkB_SHfTTdox-uphb7gvy9ABdE9py6nxQxjE7xhjk). Unzip both into the
folder of `Grandia3.exe` so that a `textures` folder sits next to it; F8 switches the textures on and
off. The pack only works with this version, not with PCSX2.

## Android (test version)

There is a test version for Android phones under [Releases](../../releases) (marked as pre-release,
file `Grandia3-Android-<version>.apk`). Its version number shows the PC version it matches: Android
1.0.7-beta1 has the features of PC v1.0.7.

- As on the PC you need your own disc image: at the first start the app asks for your Grandia III (USA)
  disc 1 ISO (choose it in the file picker, e.g. from your Download folder). It is read where it is,
  not copied.
- Install the APK directly (allow installing apps from your browser or file manager). There is no
  Play Store version.
- Touch controls are shown over the picture (they hide while a controller is used). The settings are
  behind the three-bar button at the top, or SELECT+START.
- So far it has only been tested on one phone (Snapdragon 8 Elite Gen 5). Phones with an older
  Snapdragon may be too slow; Exynos, Tensor and Dimensity phones are untested. Please report how it
  runs on yours in the [Issues](../../issues) (phone model, chip, what you tried).

## Problems?

The game writes `grandia3.log` next to itself. Please attach it when you report a problem in the
[Issues](../../issues). Feedback on the subtitles (timing, wrong or missing lines) and the translations
is welcome too.

**Black screen, then the game closes (NVIDIA):** a forced *Antialiasing - Transparency* (supersampling) in the NVIDIA Control Panel crashes the graphics driver with this game. Set it to *Off* (or *Multisampling*) for `Grandia3.exe`, or reset the program settings for it. Thanks to GothicIII for finding this ([#20](../../issues/20)).

## License

The code in this repository (the translation tools and the runtime) is under the MIT license - see
[LICENSE](LICENSE). It covers only what was written for this project: *Grandia III* and everything in it
(game code, text, audio, images, trademarks) belongs to Square Enix and Game Arts; nothing here grants
rights to it, and the language and subtitle files are fan translations of game content for private use.

## Credits

- Widescreen patch values from the PCSX2 patch database (SLUS-21334, by nemesis2000)
- Built with LLVM-MinGW and Ninja
