#include "menu.h"

#include "overlay.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


static const int MAX_ROWS = 13;            // The body's height, in rows.
static const uint32_t PANEL = 0xD0000000, LINE = 0x30FFFFFF, SELECTED = 0x38FFFFFF, TAB_ON = 0x38FFE080;

void Menu::Open()
{
	m_open = true;
	m_row = 0;
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
	m_rowH = floorf( m_h * 1.55F );
	m_pad = floorf( m_h * 0.6F );

	// Keys : pages, then rows (wrapping), from what was declared last frame.
	if ( m_pageCount > 0 ) {
		int page = (m_page + m_in.nextPage - m_in.prevPage) % m_pageCount;
		if ( page < 0 ) page += m_pageCount;
		if ( page != m_page ) { m_page = page; m_row = 0; m_rowCount = 0; }
	}
	if ( m_rowCount > 0 ) {
		m_row = (m_row + m_in.down - m_in.up) % m_rowCount;
		if ( m_row < 0 ) m_row += m_rowCount;
	}

	float w = m_h * 40.0F;
	if ( w > screenW - 2.0F * m_h ) w = screenW - 2.0F * m_h;
	float h = m_rowH * (MAX_ROWS + 4) + m_h * 1.15F + m_pad;   // Title, body, two lines of hint, keys.
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
	m_declRow = 0;
	m_declSelectable = 0;
	m_onPage = false;
	m_hint = 0;
}

bool Menu::Page( const char* name )
{
	int index = m_declPage++;
	Rect tab = { m_tabs.x, m_tabs.y + index * m_rowH, m_tabs.w, m_rowH };
	if ( m_in.mouseClicked && Hit( tab ) && index != m_page ) {
		m_page = index;
		m_row = 0;
		m_rowCount = 0;
	}
	bool current = index == m_page;
	if ( current ) OverlayRect( tab.x, tab.y, tab.w, tab.h, TAB_ON );
	else if ( Hit( tab ) ) OverlayRect( tab.x, tab.y, tab.w, tab.h, LINE );
	OverlayText( tab.x + m_pad, tab.y + (m_rowH - m_h) * 0.5F, name, current ? GOLD : 0xFFD8D8D8 );
	m_onPage = current;
	if ( current ) { m_declRow = 0; m_declSelectable = 0; }
	return current;
}

Menu::Rect Menu::NextRow()
{
	Rect r = { m_body.x, m_body.y + m_declRow * m_rowH, m_body.w, m_rowH };
	m_declRow++;
	return r;
}

void Menu::Header( const char* text )
{
	Rect r = NextRow();
	OverlayText( r.x, r.y + (m_rowH - m_h) * 0.5F, text, GOLD );
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

bool Menu::Action( const char* label, const char* hint )
{
	Rect r; bool clicked;
	bool selected = Selectable( hint, &r, &clicked );
	OverlayText( r.x, r.y + (m_rowH - m_h) * 0.5F, label, GOLD );
	return clicked || (selected && m_in.accept);
}

void Menu::End()
{
	m_pageCount = m_declPage;
	m_rowCount = m_declSelectable;
	if ( m_row >= m_rowCount ) m_row = m_rowCount > 0 ? m_rowCount - 1 : 0;

	float y = m_body.y + m_body.h + m_pad * 0.5F;
	OverlayRect( m_panel.x, y, m_panel.w, 1.0F, LINE );
	// The selected row's hint, on up to two lines.
	float top = y + (m_rowH - m_h) * 0.5F;
	if ( m_hint ) {
		char line[2][160] = { "", "" };
		int n = 0;
		const char* word = m_hint;
		while ( *word && n < 2 ) {
			const char* end = word;
			while ( *end && *end != ' ' ) end++;
			char trial[160];
			sprintf_s( trial, "%s%s%.*s", line[n], line[n][0] ? " " : "", (int) (end - word), word );
			if ( line[n][0] && OverlayTextWidth( trial ) > m_panel.x + m_panel.w - m_pad - m_body.x ) { n++; continue; }
			strcpy_s( line[n], trial );
			word = *end ? end + 1 : end;
		}
		for ( int i = 0; i < 2; i++ ) if ( line[i][0] ) OverlayText( m_body.x, top + i * m_h * 1.15F, line[i], DIM );
	}
	OverlayText( m_panel.x + m_pad, top + m_h * 1.15F + m_rowH, "Arrows : move, change    Tab : next page    Esc : close    Mouse : click", DIM );

	if ( m_in.back ) Close();
}
