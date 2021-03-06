#include "instrumentgraph.hh"
#include "i18n.hh"
#include "glutil.hh"

//const unsigned InstrumentGraph::max_panels = 10; // Maximum number of arrow lines / guitar frets


InstrumentGraph::InstrumentGraph(Audio& audio, Song const& song, input::DevType inp):
  m_audio(audio), m_song(song), m_input(input::DevType(inp)),
  m_stream(),
  m_cx(0.0, 0.2), m_width(0.5, 0.4),
  m_menu(),
  m_button(getThemePath("button.svg")),
  m_arrow_up(getThemePath("arrow_button_up.svg")),
  m_arrow_down(getThemePath("arrow_button_down.svg")),
  m_arrow_left(getThemePath("arrow_button_left.svg")),
  m_arrow_right(getThemePath("arrow_button_right.svg")),
  m_text(getThemePath("sing_timetxt.svg"), config["graphic/text_lod"].f()),
  m_selectedTrack(""),
  m_selectedDifficulty(0),
  m_rejoin(false),
  m_leftymode(false),
  m_pads(),
  m_correctness(0.0, 5.0),
  m_score(),
  m_scoreFactor(),
  m_starmeter(),
  m_streak(),
  m_longestStreak(),
  m_bigStreak(),
  m_countdown(3), // Display countdown 3 secs before note start
  m_jointime(getNaN()),
  m_dead(),
  m_ready()
{
	m_popupText.reset(new SvgTxtThemeSimple(getThemePath("sing_popup_text.svg"), config["graphic/text_lod"].f()));
	m_menuTheme.reset(new ThemeInstrumentMenu());
	for (size_t i = 0; i < max_panels; ++i) m_pressed[i] = false;
}


void InstrumentGraph::setupPauseMenu() {
	m_menu.clear();
	m_menu.add(MenuOption(_("Resume"), _("Back to performing!")));
	m_menu.add(MenuOption(_("Rejoin"), _("Change selections"), &m_rejoin));
	m_menu.add(MenuOption(_("Restart"), _("Start the song\nfrom the beginning"), "Sing"));
	m_menu.add(MenuOption(_("Quit"), _("Exit to song browser"), "Songs"));
}


void InstrumentGraph::doUpdates() {
	if (!menuOpen() && !m_ready) {
		m_ready = true;
		setupPauseMenu();
	}
}


void InstrumentGraph::toggleMenu(int forcestate) {
	if (forcestate == 1) { m_menu.open(); return; }
	else if (forcestate == 0) { m_menu.close(); return; }
	m_menu.toggle();
}


void InstrumentGraph::drawMenu() {
	ViewTrans view(m_cx.get(), 0.0, 0.75);  // Apply a per-player local perspective
	if (m_menu.empty()) return;
	Dimensions dimensions(1.0); // FIXME: bogus aspect ratio (is this fixable?)
	if (getGraphType() == input::DANCEPAD) dimensions.screenTop().middle().stretch(m_width.get(), 1.0);
	else dimensions.screenBottom().middle().fixedWidth(std::min(m_width.get(), 0.5));
	ThemeInstrumentMenu& th = *m_menuTheme;
	th.back_h.dimensions.fixedHeight(0.08f);
	m_arrow_up.dimensions.stretch(0.05, 0.05);
	m_arrow_down.dimensions.stretch(0.05, 0.05);
	m_arrow_left.dimensions.stretch(0.05, 0.05);
	m_arrow_right.dimensions.stretch(0.05, 0.05);
	MenuOptions::const_iterator cur = static_cast<MenuOptions::const_iterator>(&m_menu.current());
	double w = m_menu.dimensions.w();
	const float s = std::min(m_width.get(), 0.5) / w;
	Transform trans(glmath::scale(s));  // Fit better menu on screen
	// We need to multiply offset by inverse scale factor to keep it always constant
	// All these vars are ultimately affected by the scaling matrix
	const float txth = th.option_selected.h();
	const float button_margin = m_arrow_up.dimensions.w()
		* (m_input.isKeyboard() && getGraphType() != input::DANCEPAD ? 2.0f : 1.0f);
	const float step = txth * 0.7f;
	const float h = m_menu.getOptions().size() * step + step;
	float y = -h * .5f + step;
	float x = -w*.5f + step + button_margin;
	float xx = w*.5f - step - button_margin;
	// Background
	th.bg.dimensions.middle().center().stretch(w, h);
	th.bg.draw();
	// Loop through menu items
	w = 0;
	unsigned i = 0;
	for (MenuOptions::const_iterator it = m_menu.begin(); it != m_menu.end(); ++it, ++i) {
		std::string menutext = it->getName();
		SvgTxtTheme* txt = &th.option_selected; // Default: font for selected menu item

		if (cur != it) { // Unselected menuoption
			txt = &(th.getCachedOption(menutext));

		// Selected item
		} else {
			// Left/right Icons
			if (getGraphType() == input::DRUMS) {
				// Drum colors are mirrored
				m_arrow_left.dimensions.middle(xx + button_margin).center(y);
				m_arrow_right.dimensions.middle(x - button_margin).center(y);
			} else {
				m_arrow_left.dimensions.middle(x - button_margin).center(y);
				m_arrow_right.dimensions.middle(xx + button_margin).center(y);
			}
			m_arrow_left.draw();
			m_arrow_right.draw();

			// Up/down icons
			if (getGraphType() != input::GUITAR) {
				if (i > 0) { // Up
					m_arrow_up.dimensions.middle(x - button_margin).center(y - step);
					m_arrow_up.draw();
				}
				if (i < m_menu.getOptions().size()-1) { // Down
					m_arrow_down.dimensions.middle(x - button_margin).center(y + step);
					m_arrow_down.draw();
				}
			}

			// Draw the key letters for keyboard (not for dancepad)
			if (m_input.isKeyboard() && getGraphType() != input::DANCEPAD) {
				float leftx = x - button_margin*0.75f;
				float rightx = xx + button_margin*0.25f;
				{
					std::string hintletter = (getGraphType() == input::GUITAR ? (m_leftymode.b() ? "Z" : "1") : "U");
					SvgTxtTheme& hintfont = th.getCachedOption(hintletter);
					hintfont.dimensions.left(leftx).center(y);
					hintfont.draw(hintletter);
				}{
					std::string hintletter = (getGraphType() == input::GUITAR ? (m_leftymode.b() ? "X" : "2") : "P");
					SvgTxtTheme& hintfont = th.getCachedOption(hintletter);
					hintfont.dimensions.right(rightx).center(y);
					hintfont.draw(hintletter);
				}
				// Only drums has up/down
				if (getGraphType() == input::DRUMS) {
					if (i > 0) {  // Up
						SvgTxtTheme& hintfont = th.getCachedOption("I");
						hintfont.dimensions.left(leftx).center(y - step);
						hintfont.draw("I");
					}
					if (i < m_menu.getOptions().size()-1) {  // Down
						SvgTxtTheme& hintfont = th.getCachedOption("O");
						hintfont.dimensions.left(leftx).center(y + step);
						hintfont.draw("O");
					}
				}
			}
		}
		// Finally we are at the actual menu item text drawing
		ColorTrans c(Color::alpha(it->isActive() ? 1.0 : 0.5));
		txt->dimensions.middle(x).center(y);
		txt->draw(menutext);
		w = std::max(w, txt->w() + 2 * step + button_margin * 2); // Calculate the widest entry
		y += step; // Move draw position down for the next option
	}
	// Draw comment text
	if (cur->getComment() != "") {
		//th.comment_bg.dimensions.middle().screenBottom(-0.2);
		//th.comment_bg.draw();
		th.comment.dimensions.middle().screenBottom(-0.12);
		th.comment.draw(cur->getComment());
	}
	// Save the calculated menu dimensions
	m_menu.dimensions.stretch(w, h);
}


void InstrumentGraph::drawPopups() {
	for (Popups::iterator it = m_popups.begin(); it != m_popups.end(); ) {
		if (!it->draw()) { it = m_popups.erase(it); continue; }
		++it;
	}
}


void InstrumentGraph::handleCountdown(double time, double beginTime) {
	if (!dead() && time < beginTime && time >= beginTime - m_countdown - 1) {
		m_popups.push_back(Popup(m_countdown > 0 ?
		  std::string("- ") +boost::lexical_cast<std::string>(unsigned(m_countdown))+" -" : "Rock On!",
		  Color(0.0, 0.0, 1.0), 2.0, m_popupText.get()));
		  --m_countdown;
	}
}


Color const& InstrumentGraph::color(int fret) const {
	static Color fretColors[5] = {
		Color(0.0, 0.9, 0.0),
		Color(0.9, 0.0, 0.0),
		Color(0.9, 0.9, 0.0),
		Color(0.0, 0.0, 1.0),
		Color(0.9, 0.4, 0.0)
	};
	if (fret < 0 || fret >= m_pads) throw std::logic_error("Invalid fret number in InstrumentGraph::color");
	if (getGraphType() == input::DRUMS) {
		if (fret == 0) fret = 4;
		else if (fret == 4) fret = 0;
	}
	return fretColors[fret];
}


void InstrumentGraph::unjoin() {
	m_jointime = getNaN();
	m_rejoin = false;
	m_score = 0;
	m_starmeter = 0;
	m_streak = 0;
	m_longestStreak = 0;
	m_bigStreak = 0;
	m_countdown = 3;
	m_ready = false;
}
