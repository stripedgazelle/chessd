# chessd
This is an http server written in C for hosting correspondence chess matches.

After starting "./chessd 8080" (or whichever port you like) navigate in a browser to http://localhost:8080

Type name of player as letters only, no spaces (backspace will start again from scratch) to filter matches in progress between different players by player name.
Clicking on the name filter you typed takes you to a filtered list of matches with links to create games for any match pair.
Each game can be started as classical, chess960, or dynamite (like chess960 but with no restriction on king placement and no castling allowed).
Each match pair is persisted as a file within a newly created ./.chessd/ directory.
Typing during a game simply becomes chat.
Generally on any page ESCAPE should show a popup describing key bindings.
Move each piece by clicking on it and then clicking on the destination square (not by dragging).
Preferences are associated with each I.P. address; they include background-color, color, scale, and password.
Any side of any board can be claimed as one's own by clicking 'lock' and entering a password if one is not already set on the preferences page (every board is locked separately, even when the same named player exists on multiple; I would recommend using the same password everywhere you play in order not to forget, and using the password on the preferences page definitely helps with that!)
Clicking 'restart' returns the game to the starting position; clicking 'blank' clears the chat; after both of those, 'reset' can be clicked also if you want to remove the passwords from that board.

On each match with the named player as both white and black, a FICS feature to connect to the Free Internet Chess Server is available.
This feature is a work in progress -- currently no clocks are shown, so it is easy to lose on time, and the FICS player names are not yet transferred to the board.
