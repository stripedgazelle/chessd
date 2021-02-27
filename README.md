# chessd
This is an http server written in C for hosting correspondence chess matches.

After starting "./chessd 8080" (or whichever port you like) navigate in a browser to http://localhost:8080

Type name of player as letters only, no spaces (backspace will start again from scratch) to filter matches in progress between different players by player name.
Clicking on the name filter you typed takes you to a filtered list of matches with links to create match games.
Each game can be started as classical, chess960, or dynamite (like chess960 but with no restriction on king placement and no castling allowed).
Match games are persisted as files (one file per pairing) within a newly created ./.chessd/ directory.
Typing during a game simply becomes chat.
Generally on any page ESCAPE should show a popup describing key bindings.
Move each piece by clicking on it and then clicking on the destination square (not by dragging).
Any side of any board can be claimed as one's own by clicking 'play' and entering a password (every board is secured separately, even when the same named player exists on multiple; I would recommend using the same password everywhere you play in order not to forget!)
Clicking 'restart' returns the game to the starting position; clicking 'blank' clears the chat; after both of those, 'reset' can be clicked also if you want to remove the passwords.

On each match with the named player as both white and black, a FICS feature to connect to the Free Internet Chess Server is available.
This feature is a work in progress -- currently no clocks are shown, so it is easy to lose on time, and the FICS player names are not yet transferred to the board.
