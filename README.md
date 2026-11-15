# Pebble Fit

A simple workout app for Pebble smartwatches, designed to be read at a glance
while you move. Distance, time and steps are shown as large numbers so you can
see them even with your arm swinging.

## Features

- **Workout types:** Treadmill, Run, Walk and Bike.
- **Big numbers:** distance (miles), elapsed time and steps. Round and
  Time 2 watches use the largest system font.
- **GPS distance** for Run, Walk and Bike, using the phone's location. The
  header shows "GPS..." until the phone has a good fix.
- **Treadmill distance** calculated from your step count and your own
  "steps per mile" setting.
- **Select** pauses and resumes. While paused, **Up** ends the workout (the END
  button) and **Select** resumes it (GO). Back pauses a running workout instead
  of exiting, so a workout can't be lost by accident.
- **Settings** on the phone: steps per mile (500-4000) and background, text and
  accent colors (color watches only).

## Usage

1. Install the app and open it from the watch launcher.
2. Pick a workout. Treadmill is first in the list and shows your current steps
   per mile.
3. Press **Select** to pause or resume. When paused, press **Up** to end the
   workout; the final numbers stay on screen until you press Select or Back.

Settings are in the Pebble phone app: open Pebble Fit and choose its settings.
Outdoor workouts need the phone nearby with location permission granted.

## How it works

- The watch (`src/c/pebble-fit.c`) runs the timer and UI, counts steps from the
  Pebble Health service, and computes treadmill distance.
- The phone (`src/pkjs/index.js`) watches GPS position while a workout is
  running and sends cumulative distance to the watch over AppMessage. It
  filters low-accuracy fixes and small jitter, and ignores movement while
  paused.
- The settings page (`src/pkjs/config.js`) uses
  [`@rebble/clay`](https://github.com/pebble-dev/clay). Settings are saved on
  the watch.

## Building

Requires the [Pebble SDK](https://developer.repebble.com) and `npm`.

```sh
npm install                           # installs @rebble/clay
pebble build                          # build for all target platforms
pebble install --emulator gabbro      # run in the Round 2 emulator
pebble install --phone <ip>           # install to a paired phone
pebble screenshot --scale 3 --no-open screenshot.png
```

The emulator has no GPS or step data, so distance and steps stay at zero
there. Test those on a real watch.

## Target platforms

gabbro (Pebble Round 2), emery (Pebble Time 2), chalk (Pebble Time Round),
basalt (Pebble Time), diorite (Pebble 2) and flint (Pebble 2 Duo). aplite
(Pebble classic) is not supported because it has no Health service for step
counting.

## Project layout

```
src/c/           Watch app (C)
src/pkjs/        Phone-side JS: GPS tracking and the Clay settings page
resources/       Launcher icon (color and black & white versions)
screenshots/     Appstore screenshots, one set per platform
store/           Appstore listing icons (80x80, 144x144)
package.json     Project metadata (UUID, platforms, message keys, resources)
```

## License

No license has been chosen yet.
