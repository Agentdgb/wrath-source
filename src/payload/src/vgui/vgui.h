#pragma once

namespace dt::vgui {

// Arm the in-game overlay: hooks wglSwapBuffers and subclasses the game window.
// Returns false if either could not be installed. Does NOT block — the gui
// initialises lazily on the game's render thread a few frames later.
bool start(void* game_window);

// Tear the overlay down and remove the hooks. Blocks (bounded) until the render
// thread has released its GL objects, because they cannot be freed from here.
void stop();

// true once the gui has initialised inside the game's frame.
bool isRunning();

// Show/hide the panel. Also hands the mouse pointer back and forth between the
// panel and the game, which is why callers must go through here rather than
// poking the app's visibility directly.
void toggleVisible();
bool isVisible();

// Keys the overlay owns. The window subclass swallows them unconditionally so
// Minecraft never reacts to them, and they are read back through
// GetAsyncKeyState instead of the Java key path — which goes silent exactly
// when the panel has focus.
void setHotkeys(int toggleVk, int panicVk);

void setStreamProof(bool enabled);
bool isStreamProof();

}
