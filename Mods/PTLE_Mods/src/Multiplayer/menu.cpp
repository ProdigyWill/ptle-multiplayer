#include "menu.h"

#include "overlay.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const int MAX_ROWS = 13;            // The body's height, in rows.
static const int HINT_LINES = 3;           // The footer's hint, at most.
static const float LINE_GAP = 1.3F;        // Footer lines, in text heights.
static const uint32_t PANEL = 0xD0000000, LINE = 0x30FFFFFF, SELECTED = 0x38FFFFFF, TAB_ON = 0x38FFE080;

void Menu::Open()
{
	m_open = true;
	m_row = 0;
	m_editing = false;
}

void Menu::Close()
{
	m_open = false;
}

bool Menu::Hit( const Rect& r ) const
{
	return m_in.mouseInside && m_in.mouseX >= r.x && m_in.mouseX < r.x + r.w && m_in.mouseY >= r.y && m_in.mouseY < r.y + r.h;
}

void Menu::Begin( const MenuInput& input, float screenW, float screenH, float textHeight, const char* title, const char* status )
{
	m_in = input;
	m_h = textHeight;
	m_keepOpen = false;
	m_copy[0] = 0;
	if ( m_editing ) m_in.up = m_in.down = m_in.left = m_in.right = m_in.nextPage = m_in.prevPage = 0;   // Typing : the keys are the text's.
	m_rowH = floorf( m_h * 1.55F );
	m_pad = floorf( m_h * 0.6F );

	// Keys : pages, then rows (wrapping), from what was declared last frame.
	if ( m_pageCount > 0 ) {
		int from = m_page >= 0 ? m_page : (m_in.nextPage ? -1 : 0);   // (No page showing : Tab, the first; Shift + Tab, the last.)
		int page = m_page < 0 && !m_in.nextPage && !m_in.prevPage ? -1 : (from + m_in.nextPage - m_in.prevPage) % m_pageCount;
		if ( page < 0 && (m_in.nextPage || m_in.prevPage) ) page += m_pageCount;
		if ( page != m_page ) { m_page = page; m_row = 0; m_rowCount = 0; }
	}
	if ( m_rowCount > 0 ) {
		m_row = (m_row + m_in.down - m_in.up) % m_rowCount;
		if ( m_row < 0 ) m_row += m_rowCount;
	}

	float w = m_h * 40.0F;
	if ( w > screenW - 2.0F * m_h ) w = screenW - 2.0F * m_h;
	float h = m_rowH + m_pad * 0.5F + m_rowH * MAX_ROWS + m_pad * 0.5F   // Title, body,
		+ m_pad * 0.8F + m_h * LINE_GAP * HINT_LINES + m_pad * 0.6F + m_h + m_pad;   // the hint's lines, the keys.
	m_panel.x = floorf( (screenW - w) * 0.5F );
	m_panel.y = floorf( (screenH - h) * 0.5F );
	m_panel.w = floorf( w );
	m_panel.h = floorf( h );
	m_tabs.x = m_panel.x;
	m_tabs.y = m_panel.y + m_rowH + m_pad * 0.5F;
	m_tabs.w = floorf( m_h * 8.5F );
	m_tabs.h = m_rowH * MAX_ROWS;
	m_body.x = m_tabs.x + m_tabs.w + m_pad;
	m_body.y = m_tabs.y;
	m_body.w = m_panel.x + m_panel.w - m_pad - m_body.x;
	m_body.h = m_tabs.h;

	OverlayRect( m_panel.x, m_panel.y, m_panel.w, m_panel.h, PANEL );
	OverlayText( m_panel.x + m_pad, m_panel.y + (m_rowH - m_h) * 0.5F, title, GOLD );
	if ( status ) OverlayText( m_panel.x + m_panel.w - m_pad - OverlayTextWidth( status ), m_panel.y + (m_rowH - m_h) * 0.5F, status, DIM );
	OverlayRect( m_panel.x, m_panel.y + m_rowH, m_panel.w, 1.0F, LINE );
	OverlayRect( m_tabs.x + m_tabs.w, m_panel.y + m_rowH, 1.0F, m_tabs.h + m_pad, LINE );

	m_declPage = 0;
	m_declY = 0.0F;
	m_declSelectable = 0;
	m_declTab = 0;
	m_declGroup = 0;
	m_group = -1;
	m_onPage = false;
	m_hint = 0;
	for ( int i = 0; i < MAX_PAGES; i++ ) m_nextTabY[i] = 0.0F;
	for ( int i = 0; i < MAX_GROUPS; i++ ) m_nextGroupY[i] = 0.0F;
}

Menu::Rect Menu::NextTab()
{
	Rect tab = { m_tabs.x, m_tabs.y + m_declTab * m_rowH, m_tabs.w, m_rowH };
	m_declTab++;
	return tab;
}

void Menu::Group( const char* name )
{
	if ( m_group >= 0 ) EndGroup();
	if ( m_declGroup >= MAX_GROUPS ) return;
	int g = m_group = m_declGroup++;
	int first = m_groupFirst[g] = m_declPage;
	int count = m_groupCount[g];   // (Last frame's.)
	bool holdsCurrent = m_page >= first && m_page < first + count;
	Rect tab = NextTab();
	bool shown = m_groupOpened[g] || holdsCurrent;
	if ( m_in.mouseClicked && Hit( tab ) ) {
		if ( !shown ) {   // Open : its first page.
			m_groupOpened[g] = shown = true;
			m_page = first;
			m_row = 0;
			m_rowCount = 0;
		}
		else {   // Closed, and none of its pages showing.
			m_groupOpened[g] = shown = false;
			if ( holdsCurrent ) { m_page = -1; m_row = 0; m_rowCount = 0; holdsCurrent = false; }
		}
	}
	m_groupShown[g] = shown;
	m_nextGroupY[g] = tab.y + tab.h * 0.5F;
	if ( Hit( tab ) ) OverlayRect( tab.x, tab.y, tab.w, tab.h, LINE );
	float y = tab.y + (m_rowH - m_h) * 0.5F;
	OverlayText( tab.x + m_pad, y, name, holdsCurrent ? GOLD : 0xFFD8D8D8 );
	// A small triangle : pointing down when open, right when closed.
	// Its middle at the middle of the capitals (a bit below the text's box's).
	float size = floorf( m_h * 0.45F ), cx = tab.x + tab.w - m_pad - size, cy = floorf( y + m_h * 0.62F - size * 0.5F );
	for ( int i = 0; i < (int) size; i++ ) {
		float f = (float) i / size, len = floorf( size * (1.0F - f) );
		if ( shown ) OverlayRect( cx + floorf( (size - len) * 0.5F ), cy + i, len, 1.0F, DIM );   // Rows, narrowing downwards.
		else OverlayRect( cx + i, cy + floorf( (size - len) * 0.5F ), 1.0F, len, DIM );           // Columns, narrowing rightwards.
	}
}

void Menu::EndGroup()
{
	if ( m_group < 0 ) return;
	m_groupCount[m_group] = m_declPage - m_groupFirst[m_group];
	if ( !m_groupShown[m_group] ) m_groupOpened[m_group] = false;
	m_group = -1;
}

bool Menu::Page( const char* name )
{
	int index = m_declPage++;
	bool inGroup = m_group >= 0;
	if ( !inGroup || m_groupShown[m_group] ) {   // (In a closed group : no tab.)
		Rect tab = NextTab();
		if ( m_in.mouseClicked && Hit( tab ) && index != m_page ) {
			m_page = index;
			m_row = 0;
			m_rowCount = 0;
		}
		bool current = index == m_page;
		if ( current ) OverlayRect( tab.x, tab.y, tab.w, tab.h, TAB_ON );
		else if ( Hit( tab ) ) OverlayRect( tab.x, tab.y, tab.w, tab.h, LINE );
		float indent = inGroup ? m_pad * 2.0F : m_pad;
		OverlayText( tab.x + indent, tab.y + (m_rowH - m_h) * 0.5F, name, current ? GOLD : 0xFFD8D8D8 );
		if ( index < MAX_PAGES ) m_nextTabY[index] = tab.y + tab.h * 0.5F;
	}
	bool current = index == m_page;
	m_onPage = current;
	if ( current ) { m_declY = 0.0F; m_declSelectable = 0; m_columnCount = 0; }
	return current;
}

Menu::Rect Menu::NextRow( float height )
{
	if ( height <= 0.0F ) height = m_rowH;
	Rect r = { m_body.x, floorf( m_body.y + m_declY ), m_body.w, height };
	m_declY += height;
	return r;
}

void Menu::Section( const char* title, const char* const* columns, int count )
{
	if ( m_declY > 0.0F ) m_declY += floorf( m_rowH * 0.5F );   // Room above, after other rows.
	Rect r = NextRow();
	float y = r.y + (m_rowH - m_h) * 0.5F;
	char caps[64];
	int n = 0;
	for ( const char* c = title; *c && n < 63; c++ ) caps[n++] = (char) toupper( (unsigned char) *c );
	caps[n] = 0;
	OverlayText( r.x, y, caps, GOLD );
	if ( columns && count > 0 && m_columnCount > 0 ) {   // The columns' titles.
		for ( int i = 1; i < count && i < m_columnCount; i++ ) {
			float x = i == m_columnCount - 1 ? r.x + r.w - OverlayTextWidth( columns[i] ) : r.x + r.w * m_columns[i];
			OverlayText( x, y, columns[i], DIM );
		}
	}
	OverlayRect( r.x, r.y + r.h - 2.0F, r.w, 1.0F, LINE );
}

void Menu::Columns( const float* at, int count )
{
	m_columnCount = count < MAX_COLUMNS ? count : MAX_COLUMNS;
	for ( int i = 0; i < m_columnCount; i++ ) m_columns[i] = at[i];
}

void Menu::TableRow( const char* const* cells, int count, uint32_t color )
{
	Rect r = NextRow( floorf( m_h * 1.25F ) );
	float y = r.y + (r.h - m_h) * 0.5F;
	for ( int i = 0; i < count && i < m_columnCount; i++ ) {
		if ( !cells[i] || !cells[i][0] ) continue;
		float x = i == m_columnCount - 1 && i > 0 ? r.x + r.w - OverlayTextWidth( cells[i] ) : r.x + r.w * m_columns[i];
		OverlayText( x, y, cells[i], color );
	}
}

void Menu::Info( const char* left, const char* right, uint32_t color )
{
	Rect r = NextRow();
	float y = r.y + (m_rowH - m_h) * 0.5F;
	if ( left ) OverlayText( r.x, y, left, color );
	if ( right ) OverlayText( r.x + r.w - OverlayTextWidth( right ), y, right, color );
}

bool Menu::Selectable( const char* hint, Rect* row, bool* clicked )
{
	*row = NextRow();
	int index = m_declSelectable++;
	bool over = Hit( *row );
	if ( over && (m_in.mouseMoved || m_in.mouseClicked) ) m_row = index;   // The mouse selects what it points at.
	*clicked = over && m_in.mouseClicked;
	bool selected = index == m_row;
	if ( selected ) {
		OverlayRect( row->x - m_pad * 0.5F, row->y, row->w + m_pad, row->h, SELECTED );
		m_hint = hint;
	}
	return selected;
}

void Menu::Label( const Rect& row, const char* label, bool selected )
{
	OverlayText( row.x, row.y + (m_rowH - m_h) * 0.5F, label, WHITE );
}

bool Menu::Toggle( const char* label, bool* value, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	bool changed = clicked || (selected && (m_in.accept || m_in.left || m_in.right));
	if ( changed ) *value = !*value;
	Label( r, label, selected );
	const char* text = *value ? "On" : "Off";
	OverlayText( r.x + r.w - OverlayTextWidth( text ), r.y + (m_rowH - m_h) * 0.5F, text, *value ? GREEN : DIM );
	return changed;
}

bool Menu::Choice( const char* label, int* index, const char* const* names, int count, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	if ( count <= 0 ) return false;
	if ( *index < 0 || *index >= count ) *index = 0;

	// "<  name  >" at the right : the arrows are where the mouse steps back and forth.
	float arrow = OverlayTextWidth( "<" ), gap = m_h * 0.6F, widest = 0.0F;
	for ( int i = 0; i < count; i++ ) { float w = OverlayTextWidth( names[i] ); if ( w > widest ) widest = w; }
	float right = r.x + r.w, left = right - arrow - gap - widest - gap - arrow;
	int step = 0;
	if ( selected ) step = m_in.right + m_in.accept - m_in.left;
	if ( clicked ) step = (m_in.mouseX < left + arrow + gap) ? -1 : 1;
	if ( step ) {
		*index = (*index + step) % count;
		if ( *index < 0 ) *index += count;
	}
	Label( r, label, selected );
	float y = r.y + (m_rowH - m_h) * 0.5F;
	OverlayText( left, y, "<", selected ? GOLD : DIM );
	OverlayText( right - arrow, y, ">", selected ? GOLD : DIM );
	float w = OverlayTextWidth( names[*index] );
	OverlayText( left + arrow + gap + (widest - w) * 0.5F, y, names[*index], GOLD );
	return step != 0;
}

bool Menu::Slider( const char* label, int* value, int min, int max, int step, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	int old = *value;
	float number = OverlayTextWidth( "0000" );
	Rect track = { r.x + r.w * 0.45F, r.y + m_rowH * 0.5F - 2.0F, r.w * 0.55F - number - m_pad, 4.0F };
	Rect grab = { track.x - m_pad * 0.5F, r.y, track.w + m_pad, r.h };
	if ( selected ) *value += (m_in.right - m_in.left) * step;
	if ( m_in.mouseDown && Hit( grab ) && max > min ) {   // Click or drag along the track.
		float f = (m_in.mouseX - track.x) / track.w;
		if ( f < 0.0F ) f = 0.0F;
		if ( f > 1.0F ) f = 1.0F;
		*value = min + (int) (f * (max - min) / step + 0.5F) * step;
	}
	if ( *value < min ) *value = min;
	if ( *value > max ) *value = max;

	Label( r, label, selected );
	float f = max > min ? (float) (*value - min) / (float) (max - min) : 0.0F;
	OverlayRect( track.x, track.y, track.w, track.h, 0x50FFFFFF );
	OverlayRect( track.x, track.y, track.w * f, track.h, GOLD );
	OverlayRect( track.x + track.w * f - 2.0F, track.y - 4.0F, 4.0F, track.h + 8.0F, WHITE );
	char text[16];
	sprintf_s( text, "%d", *value );
	OverlayText( r.x + r.w - OverlayTextWidth( text ), r.y + (m_rowH - m_h) * 0.5F, text, WHITE );
	return *value != old;
}

bool Menu::Swatches( const char* label, int* index, const uint32_t* colors, int count, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	if ( count <= 0 ) return false;
	int old = *index;
	if ( selected && m_in.right != m_in.left ) {   // (-1 : none picked yet.)
		*index = (*index + m_in.right - m_in.left) % count;
		if ( *index < 0 ) *index += count;
	}
	float size = floorf( m_h * 1.05F ), gap = floorf( m_h * 0.3F );
	float x0 = r.x + r.w - count * size - (count - 1) * gap, y = r.y + (m_rowH - size) * 0.5F;
	for ( int i = 0; i < count; i++ ) {
		Rect s = { x0 + i * (size + gap), y, size, size };
		if ( clicked && Hit( s ) ) *index = i;
	}
	Label( r, label, selected );
	for ( int i = 0; i < count; i++ ) {
		float x = x0 + i * (size + gap);
		if ( i == *index ) OverlayRect( x - 2.0F, y - 2.0F, size + 4.0F, size + 4.0F, WHITE );
		OverlayRect( x, y, size, size, colors[i] | 0xFF000000 );
	}
	return *index != old;
}

// "#RRGGBB", "RRGGBB", "0xRRGGBB", "#RGB", "R, G, B", "R G B", "rgb(R, G, B)".
static bool ParseColour( const char* text, uint8_t rgb[3] )
{
	char s[64]; int n = 0;
	for ( const char* c = text; *c && n < 63; c++ ) s[n++] = (char) tolower( (unsigned char) *c );
	s[n] = 0;
	while ( n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t') ) s[--n] = 0;
	int numbers[3] = { 0, 0, 0 }, count = 0;
	bool decimal = false, letters = false;
	for ( const char* c = s; *c; c++ ) {
		if ( *c == ',' || *c == '(' ) decimal = true;
		if ( isalpha( (unsigned char) *c ) && *c != 'x' ) letters = true;
	}
	for ( const char* c = s; *c; ) {   // The numbers in it.
		if ( !isdigit( (unsigned char) *c ) ) { c++; continue; }
		char* end;
		long v = strtol( c, &end, 10 );
		if ( count < 3 ) numbers[count] = (int) v;
		count++;
		c = end;
	}
	if ( strncmp( s, "rgb", 3 ) == 0 ) decimal = true;
	if ( !decimal && !letters && count == 3 && strchr( s, ' ' ) ) decimal = true;   // "220 50 40".
	if ( decimal ) {
		if ( count != 3 ) return false;
		for ( int i = 0; i < 3; i++ ) if ( numbers[i] < 0 || numbers[i] > 255 ) return false;
		for ( int i = 0; i < 3; i++ ) rgb[i] = (uint8_t) numbers[i];
		return true;
	}
	char hex[8]; int h = 0;
	const char* c = s;
	while ( *c == ' ' || *c == '#' ) c++;
	if ( c[0] == '0' && c[1] == 'x' ) c += 2;
	for ( ; *c && *c != ' '; c++ ) {
		if ( !isxdigit( (unsigned char) *c ) || h == 6 ) return false;
		hex[h++] = *c;
	}
	while ( *c == ' ' ) c++;
	if ( *c ) return false;
	if ( h == 3 ) { hex[4] = hex[5] = hex[2]; hex[2] = hex[3] = hex[1]; hex[1] = hex[0]; h = 6; }
	if ( h != 6 ) return false;
	hex[6] = 0;
	unsigned long v = strtoul( hex, 0, 16 );
	rgb[0] = (uint8_t) (v >> 16); rgb[1] = (uint8_t) (v >> 8); rgb[2] = (uint8_t) v;
	return true;
}

bool Menu::Colour( const char* label, uint8_t rgb[3], const char* hint )
{
	Rect r; bool clicked;
	int index = m_declSelectable;
	bool selected = Selectable( hint, &r, &clicked );
	bool editing = m_editing && m_editRow == index;
	bool changed = false;
	if ( editing && !selected ) m_editing = editing = false;   // Moved away : given up.
	if ( selected && m_in.pasted ) {
		uint8_t c[3];
		if ( ParseColour( m_in.pasted, c ) ) { memcpy( rgb, c, 3 ); changed = true; }
		m_editing = editing = false;
	}
	else if ( editing ) {
		for ( const char* t = m_in.typed; t && *t; t++ ) {
			size_t n = strlen( m_edit );
			if ( *t == '\b' ) { if ( n ) m_edit[n - 1] = 0; }
			else if ( n + 1 < sizeof(m_edit) ) { m_edit[n] = *t; m_edit[n + 1] = 0; }
		}
		if ( m_in.accept ) {
			uint8_t c[3];
			if ( !m_edit[0] ) m_editing = editing = false;
			else if ( ParseColour( m_edit, c ) ) { memcpy( rgb, c, 3 ); changed = true; m_editing = editing = false; }
		}
		if ( m_in.back ) { m_editing = editing = false; m_keepOpen = true; }
	}
	else if ( selected && (m_in.accept || clicked) ) {
		m_editing = editing = true;
		m_editRow = index;
		m_edit[0] = 0;
	}
	if ( selected && m_in.copy ) sprintf_s( m_copy, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2] );

	Label( r, label, selected );
	float y = r.y + (m_rowH - m_h) * 0.5F;
	float codeW = OverlayTextWidth( "#DDDDDD" );
	float sw = floorf( m_h * 4.0F ), sh = floorf( m_rowH - 6.0F );
	float sx = r.x + r.w - codeW - m_pad - sw;
	OverlayRect( sx - 1.0F, r.y + 2.0F, sw + 2.0F, sh + 2.0F, 0x80FFFFFF );
	OverlayRect( sx, r.y + 3.0F, sw, sh, 0xFF000000 | ((uint32_t) rgb[0] << 16) | ((uint32_t) rgb[1] << 8) | rgb[2] );
	if ( editing ) {
		char shown[40];
		uint8_t c[3];
		bool ok = !m_edit[0] || ParseColour( m_edit, c );
		sprintf_s( shown, "%s_", m_edit );
		float w = OverlayTextWidth( shown );
		if ( w < codeW ) w = codeW;
		float x = r.x + r.w - w;
		if ( x < sx - 4.0F ) {   // Long : over the swatch.
			OverlayRect( x - 6.0F, r.y + 2.0F, w + 6.0F, r.h - 4.0F, 0xE0202020 );
		}
		else OverlayRect( x - 6.0F, r.y + 2.0F, w + 6.0F, r.h - 4.0F, 0x60000000 );
		OverlayText( x, y, shown, ok ? WHITE : 0xFFFF8080 );
	}
	else {
		char code[16];
		sprintf_s( code, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2] );
		OverlayText( r.x + r.w - OverlayTextWidth( code ), y, code, GOLD );
	}
	return changed;
}

bool Menu::Button( const char* label, const char* value, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	Label( r, label, selected );
	if ( value ) OverlayText( r.x + r.w - OverlayTextWidth( value ), r.y + (m_rowH - m_h) * 0.5F, value, GOLD );
	return clicked || (selected && m_in.accept);
}

bool Menu::Action( const char* label, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	OverlayText( r.x, r.y + (m_rowH - m_h) * 0.5F, label, GOLD );
	return clicked || (selected && m_in.accept);
}

void Menu::End()
{
	if ( m_group >= 0 ) EndGroup();
	m_pageCount = m_declPage;
	m_rowCount = m_declSelectable;
	if ( m_row >= m_rowCount ) m_row = m_rowCount > 0 ? m_rowCount - 1 : 0;
	for ( int i = 0; i < MAX_PAGES; i++ ) m_tabY[i] = m_nextTabY[i];
	for ( int i = 0; i < MAX_GROUPS; i++ ) m_groupY[i] = m_nextGroupY[i];

	float y = m_body.y + m_body.h + m_pad * 0.5F;
	OverlayRect( m_panel.x, y, m_panel.w, 1.0F, LINE );
	// The selected row's hint, on up to three lines, then the keys : centred.
	float top = y + m_pad * 0.8F, width = m_panel.w - 2.0F * m_pad, mid = m_panel.x + m_panel.w * 0.5F;
	if ( m_hint ) {
		char line[HINT_LINES][200] = { "", };
		int n = 0;
		const char* word = m_hint;
		while ( *word && n < HINT_LINES ) {
			const char* end = word;
			while ( *end && *end != ' ' ) end++;
			char trial[200];
			sprintf_s( trial, "%s%s%.*s", line[n], line[n][0] ? " " : "", (int) (end - word), word );
			if ( line[n][0] && OverlayTextWidth( trial ) > width ) { n++; continue; }
			strcpy_s( line[n], trial );
			word = *end ? end + 1 : end;
		}
		int used = 0;
		for ( int i = 0; i < HINT_LINES; i++ ) if ( line[i][0] ) used = i + 1;
		float first = top + (HINT_LINES - used) * m_h * LINE_GAP * 0.5F;   // Fewer lines : in the middle of their space.
		for ( int i = 0; i < used; i++ ) OverlayText( floorf( mid - OverlayTextWidth( line[i] ) * 0.5F ), first + i * m_h * LINE_GAP, line[i], DIM );
	}
	const char* keys = m_editing ? "Type a color    Enter : set it    Esc : leave it    Ctrl + V : paste"
		: "Arrows : move, change    Tab : next page    Esc : close    Mouse : click";
	OverlayText( floorf( mid - OverlayTextWidth( keys ) * 0.5F ), top + m_h * LINE_GAP * HINT_LINES + m_pad * 0.6F, keys, 0xFF909090 );

	if ( m_in.back && !m_keepOpen ) Close();
}
