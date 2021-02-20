# chessd
This is an http server written in C for hosting correspondence chess matches.

In this initial version boards are open to anyone who has a URL.
I'll maybe make enforcement of player identity an option in future.

After starting "./chessd 8080" (or whichever port you like) navigate in a browser to http://localhost:8080

Type name of player as letters only, no spaces (backspace will start again from scratch) to filter games by player name.
Clicking on the name filter you typed takes you to a filtered list of players with settings for each.
Clicking on the name filter there takes you to a filtered list of matches with links to create matches.
Match can be started as classical, chess960, or dynamite (like chess960 but with no restriction on king placement and no castling allowed).
Matches will be persisted as files (one file per pairing) within newly created ./.chessd/ directory.
Typing during a match simply becomes chat.
Generally on any page ESCAPE should show a popup describing key bindings.

On each match with the named player as both white and black, a FICS feature to connect to the Free Internet Chess Server is available.
This feature is a work in progress -- currently no clocks are shown, so it is easy to lose on time, and the FICS player names are not yet transferred to the board.
