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
	int del;                       // Delete.
	int pageUp, pageDown;          // Page Up, Page Down (with key repeat).
	int shiftLeft, shiftRight;     // Shift + Left / Right (not in left, right).
	float mouseX, mouseY;          // Overlay pixels.
	bool mouseInside;              // Over the game's image.
	bool mouseDown;                // Left button held.
	bool mouseClicked;             // Left button went down this frame.
	bool mouseMoved;
	int wheel;                     // Mouse wheel notches (up : positive).
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
	// Where a page's tab (or a group's heading) was drawn last frame, for the tests to
	// click; false : not shown (in a closed group).
	bool TabCenter( int page, float* x, float* y ) const { return Center( page >= 0 && page < MAX_PAGES ? m_tabY[page] : -1.0F, x, y ); }
	bool GroupCenter( int group, float* x, float* y ) const { return Center( group >= 0 && group < MAX_GROUPS ? m_groupY[group] : -1.0F, x, y ); }
	bool GroupOpen( int group ) const { return group >= 0 && group < MAX_GROUPS && m_groupShown[group]; }
	// Where the last List was drawn, for the tests to point at; false : none yet.
	bool ListCenter( float* x, float* y ) const
	{
		*x = m_list.x + m_list.w * 0.5F;
		*y = m_list.y + m_list.h * 0.5F;
		return m_list.h > 0.0F;
	}
	void Open();
	void Close();

	// Narrow : the menu keeps to the left of the screen (the game shows at the right). Before Begin.
	void SetNarrow( bool narrow ) { m_narrow = narrow; }

	// status : shown at the right of the title bar (may be 0).
	void Begin( const MenuInput& input, float screenW, float screenH, float textHeight, const char* title, const char* status );

	// Declares a page. True : it's the current one, declare its rows now.
	bool Page( const char* name );
	// A heading in the page list with pages under it, declared between Group and
	// EndGroup. Clicking it shows them and opens the first; it stays open while one of
	// them is showing. Tab goes through them all the same.
	void Group( const char* name );
	void EndGroup();

	// Rows that only show something.
	void Info( const char* left, const char* right, uint32_t color = WHITE );
	// A section's title, in capitals and underlined (with some room above it, after
	// other rows). columns : the titles of a table's columns, on the same line.
	void Section( const char* title, const char* const* columns = 0, int count = 0 );
	// A table : where its columns start (fractions of the row's width; the last
	// column is put against the right edge), then its rows, closer together than
	// the settings and not selectable.
	void Columns( const float* at, int count );
	void TableRow( const char* const* cells, int count, uint32_t color = WHITE );

	// Rows the player can change (true : changed this frame). hint : a line shown
	// at the bottom while the row is selected (may be 0).
	bool Toggle( const char* label, bool* value, const char* hint = 0 );
	bool Choice( const char* label, int* index, const char* const* names, int count, const char* hint = 0 );
	bool Slider( const char* label, int* value, int min, int max, int step, const char* hint = 0 );
	bool Swatches( const char* label, int* index, const uint32_t* colors, int count, const char* hint = 0 );
	bool Action( const char* label, const char* hint = 0 );
	// A row with a value at the right; Enter or a click : true (e.g. "press a key to use").
	bool Button( const char* label, const char* value, const char* hint = 0 );
	// A colour : a swatch of it and its hex code. Enter : type a hex code or R, G, B
	// (Enter again sets it, Esc gives up); Ctrl + V pastes one; Ctrl + C copies it.
	bool Colour( const char* label, uint8_t rgb[3], const char* hint = 0 );

	// Text typed in place : Enter (or a click) starts typing, Enter or Esc stops. True
	// as it changes. placeholder : shown while it's empty (may be 0).
	bool TextField( const char* label, char* text, int size, const char* placeholder = 0, const char* hint = 0 );
	// Eight slots round a ring (a wheel's), the first at the top, clockwise; names[i] 0 :
	// empty. Left / Right move *cursor round it. Returns what the player asked for :
	// Enter or a click, choose the slot's content; Delete, empty it; Shift + Left / Right,
	// move it round (*cursor moved with it, *swapFrom where it was).
	enum { RING_NONE, RING_CHOOSE, RING_CLEAR, RING_SWAP };
	int Ring( const char* const* names, int* cursor, const char* centre, int* swapFrom, const char* hint = 0 );
	// A list scrolling through visible rows. While it's selected, Up / Down go through it
	// (out of it at its ends), Page Up / Down by pages; the mouse wheel over it scrolls it.
	// True : *cursor chosen (Enter, a click).
	bool List( const char* const* names, const char* const* tags, int count, int* cursor, int* scroll, int visible, const char* hint = 0 );
	// Esc, this frame : taken by the page (back out of something), the menu stays open.
	bool Back();
	// The footer's keys line, this frame (0 : the usual).
	void SetKeys( const char* keys ) { m_keys = keys; }

	// Text for the clipboard (Ctrl + C on a row that has some), this frame; 0 : none.
	const char* CopyText() const { return m_copy[0] ? m_copy : 0; }
	bool IsTyping() const { return m_open && m_editing; }

	// Draws the footer. The menu closes itself on Esc (IsOpen then says so).
	void End();

private:
	static const int MAX_PAGES = 16, MAX_GROUPS = 4, MAX_COLUMNS = 6;
	struct Rect { float x, y, w, h; };
	bool Center( float y, float* outX, float* outY ) const
	{
		*outX = m_tabs.x + m_tabs.w * 0.5F;
		*outY = y;
		return y > 0.0F;
	}
	Rect NextTab();
	// Starts a row the player can select (height 0 : a row's; highlight : drawn when
	// selected); true : it's the selected one.
	bool Selectable( const char* hint, Rect* row, bool* clicked, float height = 0.0F, bool highlight = true );
	Rect NextRow( float height = 0.0F );   // (0 : a row's height.)
	bool Hit( const Rect& r ) const;
	void Label( const Rect& row, const char* label, bool selected );

	bool m_open = false;
	int m_page = 0, m_row = 0;            // Current page; selected row among its selectable ones.
	int m_pageCount = 0, m_rowCount = 0;  // As declared last frame.

	// This frame.
	MenuInput m_in;
	float m_h, m_rowH, m_pad;
	Rect m_panel, m_tabs, m_body;
	Rect m_list = { 0, 0, 0, 0 };         // The last List's.
	int m_declPage, m_declSelectable, m_declTab, m_declGroup;
	float m_declY;                        // Where the next row goes, below the body's top.
	bool m_onPage;
	int m_group = -1;                     // The group being declared (-1 : none).

	// The page list, as drawn last frame (and this one's, as it's declared) : each tab's
	// middle (0 : not shown) and the groups.
	float m_tabY[MAX_PAGES] = { 0, }, m_groupY[MAX_GROUPS] = { 0, };
	float m_nextTabY[MAX_PAGES] = { 0, }, m_nextGroupY[MAX_GROUPS] = { 0, };
	bool m_groupOpened[MAX_GROUPS] = { false, };   // Opened by a click (and not closed since).
	bool m_groupShown[MAX_GROUPS] = { false, };    // Showing its pages this frame.
	int m_groupFirst[MAX_GROUPS] = { 0, }, m_groupCount[MAX_GROUPS] = { 0, };
	float m_columns[MAX_COLUMNS];
	int m_columnCount = 0;
	const char* m_hint;
	bool m_editing = false;               // Typing into the selected row (a Colour).
	int m_editRow = -1;
	char m_edit[32] = { 0, };
	bool m_keepOpen = false;              // Esc ended the typing, not the menu.
	char m_copy[32] = { 0, };
	bool m_narrow = false;
	int m_capture = -1, m_captureNext = -1;   // The list that has Up / Down (this frame, the next).
	bool m_listKeys = false;                  // ... and it was selected as the frame began.
	const char* m_keys = 0;
};
