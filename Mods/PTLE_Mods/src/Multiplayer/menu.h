#pragma once

// The in-game menu (F1) : pages down the left, the current page's rows on the
// right, driven by the keyboard and the mouse. Drawn with the overlay.
//
// It is rebuilt every frame from the code that owns the settings ("immediate
// mode") : between Begin and End, each page is declared with Page(), and if that
// returns true (it's the one showing) its rows are declared in order. A row that
// edits a value returns true on the frame the player changed it :
//
//     menu.Begin( input, w, h, textHeight, "Multiplayer", "3 players" );
//     if ( menu.Page( "Display" ) ) {
//         if ( menu.Toggle( "Name tags", &showTags, "Names above the other players." ) ) Save();
//     }
//     menu.End();
//
// Adding a setting is one line where its value lives; adding a page is one Page().

#include <stdint.h>


// What the player did since the last frame.
struct MenuInput
{
	int up, down, left, right;     // Presses (with key repeat).
	int accept, back;              // Enter, Esc.
	int nextPage, prevPage;        // Tab, Shift + Tab.
	float mouseX, mouseY;          // Overlay pixels.
	bool mouseInside;              // Over the game's image.
	bool mouseDown;                // Left button held.
	bool mouseClicked;             // Left button went down this frame.
	bool mouseMoved;
	const char* typed;             // Characters typed since the last frame ('\b' : Backspace; may be 0).
	const char* pasted;            // The clipboard's text, on Ctrl + V (may be 0).
	bool copy;                     // Ctrl + C.
};

class Menu
{
public:
	static const uint32_t WHITE = 0xFFFFFFFF, DIM = 0xFFB0B0B0, GOLD = 0xFFFFE080, GREEN = 0xFFA0FFA0;

	bool IsOpen() const { return m_open; }
	int CurrentPage() const { return m_page; }
	void TabCenter( int page, float* x, float* y ) const   // Where a page's tab was drawn last frame (tests click it).
	{
		*x = m_tabs.x + m_tabs.w * 0.5F;
		*y = m_tabs.y + (page + 0.5F) * m_rowH;
	}
	void Open();
	void Close();

	// status : shown at the right of the title bar (may be 0).
	void Begin( const MenuInput& input, float screenW, float screenH, float textHeight, const char* title, const char* status );

	// Declares a page. True : it's the current one, declare its rows now.
	bool Page( const char* name );

	// Rows that only show something.
	void Header( const char* text );
	void Info( const char* left, const char* right, uint32_t color = WHITE );

	// Rows the player can change (true : changed this frame). hint : a line shown
	// at the bottom while the row is selected (may be 0).
	bool Toggle( const char* label, bool* value, const char* hint = 0 );
	bool Choice( const char* label, int* index, const char* const* names, int count, const char* hint = 0 );
	bool Slider( const char* label, int* value, int min, int max, int step, const char* hint = 0 );
	bool Swatches( const char* label, int* index, const uint32_t* colors, int count, const char* hint = 0 );
	bool Action( const char* label, const char* hint = 0 );
	// A colour : a swatch of it and its hex code. Enter : type a hex code or R, G, B
	// (Enter again sets it, Esc gives up); Ctrl + V pastes one; Ctrl + C copies it.
	bool Colour( const char* label, uint8_t rgb[3], const char* hint = 0 );

	// Text for the clipboard (Ctrl + C on a row that has some), this frame; 0 : none.
	const char* CopyText() const { return m_copy[0] ? m_copy : 0; }
	bool IsTyping() const { return m_open && m_editing; }

	// Draws the footer. The menu closes itself on Esc (IsOpen then says so).
	void End();

private:
	struct Rect { float x, y, w, h; };
	bool Selectable( const char* hint, Rect* row, bool* clicked );   // Starts a row the player can select; true : it's the selected one.
	Rect NextRow();
	bool Hit( const Rect& r ) const;
	void Label( const Rect& row, const char* label, bool selected );

	bool m_open = false;
	int m_page = 0, m_row = 0;            // Current page; selected row among its selectable ones.
	int m_pageCount = 0, m_rowCount = 0;  // As declared last frame.

	// This frame.
	MenuInput m_in;
	float m_h, m_rowH, m_pad;
	Rect m_panel, m_tabs, m_body;
	int m_declPage, m_declRow, m_declSelectable;
	bool m_onPage;
	const char* m_hint;
	bool m_editing = false;               // Typing into the selected row (a Colour).
	int m_editRow = -1;
	char m_edit[32] = { 0, };
	bool m_keepOpen = false;              // Esc ended the typing, not the menu.
	char m_copy[32] = { 0, };
};
