# Opt85
An 8085 Optimizer For AckCC

# Status

This is a very early prototype WIP of a second stage optimizer for the ACK
C compiler 8080 output. There are several basic goals

- Remove stuff the compiler peephole can't because it has no notion of usage tracking
- Replace constants with known live register values
- Remove unneeded register loads (constants are easy, labels harder, memory and stack probably too hard to bother)
- Perform optimization transforms on the code
- Track the difference between the stack pointer and the frame pointer so we can use ldhi
- Eliminate compiler helper calls that can be swapped with ldsi etc

At the moment it is just early experimental code that can take a simple
series of 8080 opcodes, do basic value tracking on ABCDEHL and also remove
a few unneeded instructions and switch some 8bit operations on constants to
use any values lurking.

There is a lot left to do before it is even minimally useful, including getting
the fp/sp tracking done, teaching it to parse all the meta operations the
compiler outputs, memory segments etc, linking up label references and so on.

In part this is also testing out some ideas that will be needed for the 6803
C compiler work.
