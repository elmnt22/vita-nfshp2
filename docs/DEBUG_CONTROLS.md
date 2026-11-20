# Debug controls

Enable **Advanced Settings → Enable debug menu and debug cameras** in the launcher, or start with `--debug`. This does not enable content unlock automatically. Names below refer to logical game buttons; use the corresponding bindings in your input profile.

| Logical button | Common Xbox / SDL2 equivalent |
|---|---|
| Start | Menu |
| Select | Back / View |
| Cross / Circle / Square / Triangle | A / B / X / Y |
| L1 / R1 | LB / RB |
| L2 / R2 | LT / RT |
| L3 / R3 | Left / right stick click |

## Enter and leave camera control

During an ordinary single-player race, hold **Start**, then press **Select**. Release every button, trigger and stick so the input can return to neutral. `DEBUG CAMERA` indicates that camera control is active. Repeat the combination to return to driving. The previous view is restored.

Short **Select** cycles the available original cameras. The ordinary race cycle includes the game view, orbit camera, free camera and road camera. Some session/replay contexts have their own camera cycle or block camera focus. Camera focus is unavailable in the frontend, pause menu and split-screen gameplay.

Player 1 receives neutral driving input while camera control is active; the simulation continues. The car can coast and collide. Loss of window focus or the selected controller closes camera focus and requires neutral input before driving resumes. Player 2 remains independent.

With debug tools enabled in the ordinary single-player driving context, **Start without Select** opens pause after release. Free and road cameras use Start for the actions below, so leave camera focus before opening ordinary pause.

## Orbit camera

| Control | Action |
|---|---|
| D-pad left / right | Orbit horizontally |
| D-pad up / down | Change vertical angle |
| L2 / L1 | Move closer / farther |
| R1 / R2 | Next / previous world car when no fixed target is set |
| R3 | Toggle helicopter tracking when a helicopter exists |
| Select | Next camera |

## Free camera

| Control | Action |
|---|---|
| D-pad | Rotate the camera |
| Cross / Triangle | Move forward / backward |
| Square / Circle | Move sideways |
| L1 / L2 | Move up / down |
| R1 / R2 | Faster movement; R2 takes priority if both are held |
| Start, then release | Place Player 1's car in front of the camera |
| Select | Next camera |

The Start–Select combination leaves camera focus without triggering placement. Stick movement is not bound by this camera's normal activation; L3/R3 have no active action here.

## Road camera

This mode uses the live road recovery points. Changes affect the current world in memory; it is not a complete track editor and does not save modified track files.

| Control | Action |
|---|---|
| Left stick | Rotate |
| Right stick | Move across the plane |
| R3 | Faster movement and rotation |
| L1 / L2 | Move up / down |
| Triangle | Find a nearby suitable road group / point |
| R2 / R1 | Next / previous point in the group |
| Start, then release | Reset camera position relative to the selected point |
| Circle + left / right | Adjust one half of the road width |
| Square + left / right | Adjust the other half of the road width |
| Circle or Square while changing point | Carry that side's width to the selected point |
| Cross + D-pad | Move the selected point; invalid changes are rejected |
| Select | Next camera |

## Pause debug menu

Use the D-pad to select an entry and left/right to change values. The normal Back binding returns to the previous screen.

| Entry | Action |
|---|---|
| Finish in Nth position | Queues the existing special replay event for the player when the menu closes; zero leaves the result unchanged |
| Rearview Mirror | Toggles the rearview mirror |
| ENV MAPPING | Toggles environment mapping |
| Show debug Info | Toggles the four original diagnostic HUD lines |
| Super easy AI | Changes the existing AI option through the replay event path |
| Exit | Requests normal application shutdown |

Some original diagnostic entries are visible but inactive, including Speedline mode, Car-to-car collisions, Screen Print and Environment. They do not implement an additional runtime feature.

## Frame statistics

`C: …fps` represents measured CPU work; `G: …fps` uses available GPU timing and displays `n/a` when that timing is unavailable. The other original HUD lines show the car's coordinates / scenery zone and accumulated distance.

For presentation FPS, enable **Display → Show real render FPS counter** in the launcher. It works independently of debug mode and counts accepted presentation requests over elapsed time. Game simulation runs at 60 Hz regardless of the presentation rate.
