# chessd
This is an http server written in C for hosting correspondence chess matches.

After starting "./chessd 8080" (or whichever port you like) navigate in a browser to http://localhost:8080

Type name of player as letters only, no spaces (backspace will start again from scratch) to filter games in progress between that player and other players.
Clicking on 'players' takes you to a filtered list of players with links to create games for any pair.
On each game with the same player name as both white and black, that named player may be protected via a password by clicking 'claim'.
To remove that association, click 'disown'.
Clicking on 'preferences' allows you to choose background-color, color, scale, and password (or blank password to logout) for your I.P. address.
Each game can be started as classical, chess960, or dynamite (like chess960 but with no restriction on king placement and no castling allowed).
Typing during a game simply becomes chat.
Generally on any page ESCAPE should show a popup describing key bindings.
Move each piece by clicking on it and then clicking on the destination square (not by dragging).
Clicking 'restart' returns the game to the starting position; clicking 'blank' clears the chat.

Clicking 'fics' connects to the Free Internet Chess Server, but be aware that this feature is a work in progress -- currently no clocks are shown, so it is easy to lose on time, and the FICS player names are not yet transferred to the board.

Each game xxx_vs_yyy is persisted as a file .chessd/xxx_vs_yyy with positions for every move persisted in .chessd/xxx_vs_yyy.pos.
Games with the same player xxx as both white and black are named xxx rather than xxx_vs_xxx.
