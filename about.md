# xdBot

<cl>xdBot</c> is a simple botting tool designed to be easy for anyone to use; intended for <cg>showcases</c>.

# Features

 * Basic ClickBot.
 * Practice Fixes.
 * Seed Modifier.
 * Noclip.
 * Show Trajectory.
 * Layout Mode.
 * Speedhack.
 * Frame Stepper.
 * Sub-tick Stepper (this fork): see below.
 * Safe Mode.
 * Renderer.
 * Instant Respawn.
 * No Respawn Flash.
 * No Death Effect.
 * Macro saving and loading system.
 * Macro Auto Saving.

# How to use

* Open the menu using the Open Menu Keybind or the button in the Pause Menu.

* Click the 'Record' toggle in the menu to start recording.

* Finish a practice run of the level.

* Play the macro by clicking the 'Play' toggle in the menu and going into the level.

# Thanks

* Thanks to Viper for letting me use their Safe Mode implementation.

* Thanks to ReplayBot for being open source and letting me steal the code for the renderer.

* Thanks to CatXus and Aadam_yes for helping me test the early android versions.


# Sub-tick stepper (this fork)

Press <cy>Alt+V</c> in a level (or <cy>Stepper</c> in the pause menu). The level only moves when you step it, and the stepper edits the loaded macro.

* <cy>C</c> / <cy>B</c>: a frame forward / back (hold to keep going). <cy>Shift+C</c> / <cy>Shift+B</c>: a small step inside the frame.
* <cy>PageUp</c> / <cy>PageDown</c>: finer / coarser steps (240 Hz to 2,400,000,000 Hz).
* Click (or <cy>H</c>) to press or let go of jump where the stepper is: inside a frame, it lands that far into it.
* <cy>N</c>: the next click of the macro moved here. <cy>Delete</c>: the next click taken out. <cy>Ctrl+Z</c>: undo.
* <cy>P</c>: play from here (stops at a death). <cy>Shift+P</c>: back to where play started.
* The panel at the bottom: select a click on the graph and move it with <cy>Move selected</c>; tap the graph to go there, drag it to scroll.
* Save the macro as usual. <cy>CML</c> keeps the timings between frames for xdBot 2.7 and the Frame Window Counter.

Every key can be changed in the mod's settings.
