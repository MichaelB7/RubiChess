/*
  RubiChess is a UCI chess playing engine by Andreas Matthies.

  RubiChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  RubiChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "RubiChess.h"


void uci::send(const char* format, ...)
{
    va_list argptr;
    va_start(argptr, format);
    vfprintf(stdout, format, argptr);
    va_end(argptr);

    //cout << s;
}

GuiToken uci::parse(vector<string>* args, string ss)
{
    bool firsttoken = false;

    if (ss == "")
    {
        //getline(cin, ss);
        // ugly hack because getline somehow makes the flto optimized windows build crash
        const int bufsize = 4096;
        char c[bufsize];
        bool bEol = false;
        while (!bEol && fgets(c, bufsize, stdin))
        {
            size_t l = strlen(c);
            bEol = (c[l - 1] == '\n');
            if (bEol)
                c[l - 1] = 0;
            ss += c;
        }
    }

    GuiToken result = UNKNOWN;
    istringstream iss(ss);
    for (string s; iss >> s; )
    {
        if (!firsttoken)
        {
            if (GuiCommandMap.find(s.c_str()) != GuiCommandMap.end())
            {
                result = GuiCommandMap.find(s.c_str())->second;
                firsttoken = true;
            }
        }
        else {
            args->push_back(s);
        }
    }

    return result;
}
