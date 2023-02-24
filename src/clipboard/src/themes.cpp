/*  Clipboard - Cut, copy, and paste anything, anywhere, all from the terminal.
    Copyright (C) 2023 Jackson Huff and other contributors on GitHub.com
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.*/
#include "clipboard.hpp"

void setTheme(const std::string& theme) {
    if (theme == "light") {
        colors = {
                {{"[error]", "\033[38;5;196m"},
                 {"[success]", "\033[38;5;22m"},
                 {"[progress]", "\033[38;5;202m"},
                 {"[info]", "\033[38;5;20m"},
                 {"[help]", "\033[38;5;200m"},
                 {"[bold]", "\033[1m"},
                 {"[blank]", "\033[0m"}}};
    } else if (theme == "amber") {
        colors = {
                {{"[error]", "\033[38;5;202m"},
                 {"[success]", "\033[38;5;220m"},
                 {"[progress]", "\033[38;5;214m"},
                 {"[info]", "\033[38;5;222m"},
                 {"[help]", "\033[38;5;226m"},
                 {"[bold]", "\033[1m"},
                 {"[blank]", "\033[0m"}}};
    } else if (theme == "green") {
        colors = {
                {{"[error]", "\033[38;5;106m"},
                 {"[success]", "\033[38;5;41m"},
                 {"[progress]", "\033[38;5;48m"},
                 {"[info]", "\033[38;5;154m"},
                 {"[help]", "\033[38;5;46m"},
                 {"[bold]", "\033[1m"},
                 {"[blank]", "\033[0m"}}};
    }
}