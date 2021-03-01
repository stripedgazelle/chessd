# chessd
This is an http server written in C for hosting correspondence chess matches.

After starting "./chessd 8080" (or whichever port you like) navigate in a browser to http://localhost:8080

Type name of player as letters only, no spaces (backspace will start again from scratch) to filter matches in progress between different players by player name.
Clicking on the name filter you typed takes you to a filtered list of players with links to create games for any match pair.
Each game can be started as classical, chess960, or dynamite (like chess960 but with no restriction on king placement and no castling allowed).
Each match pair is persisted as a file within a newly created ./.chessd/ directory.
Typing during a game simply becomes chat.
Generally on any page ESCAPE should show a popup describing key bindings.
Move each piece by clicking on it and then clicking on the destination square (not by dragging).
Preferences are associated with each I.P. address; they include background-color, color, scale, and password.
Clicking 'restart' returns the game to the starting position; clicking 'blank' clears the chat.

On each match board with a named player as both white and black, that named player may be protected via a password by clicking 'claim'.
To remove that association, click 'disown'.
Clicking 'fics' connects to the Free Internet Chess Server, but be aware that this feature is a work in progress -- currently no clocks are shown, so it is easy to lose on time, and the FICS player names are not yet transferred to the board.
