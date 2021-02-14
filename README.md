# chessd
An http server written in C for hosting correspondence chess matches

In this initial version boards are open to anyone who has a URL.
I'll maybe make enforcement of player identity an option in future.

Games can be started as classical, chess960, or dynamite (like chess960 but with no restriction on king placement and no castling allowed).

On each board that has the same named player as both white and black, a FICS feature to connect to the Free Internet Chess Server is available.
This feature is a work in progress -- currently no clocks are shown, so it is easy to lose on time, and the FICS player names are not yet transferred to the board.
