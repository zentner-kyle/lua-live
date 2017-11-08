lua-live
========

This is a proof-of-concept modification of the [Lua programming
language](https://www.lua.org/).

It allows updating the behavior of a program while the program is executing.
Functions can even be updated if they contain loops which are currently
executing, or coroutines which are suspended in them.

Updates in this system are performed by sending the updated program source file
to the running virtual machine using ZeroMQ. The virtual machine then suspends
the executing program, and executes the updated version. Annotations in the
source code are used to record which functions are eligible for patching. After
the changes are recorded, the original program resumes executing. While it is
executing, on all backwards jumps the virtual machine attempts to transition
the current frame to an updated version of the function.

Note that as a proof-of-concept the system has several limitations. For
example, new constants cannot be used in the updated version of the function.

Example usage:

After installing ZeroMQ headers and building the system using make, load (`src/lua example/hello1.lua`) this program, which should output `Hello` repeatedly.

`hello1.lua:`
```lua
function main()
  while true do
    print('Hello')
  end
end

live.patch('main', main)

live.start(main)
```

Then, patch the program to version 2 (run `src/patch.py example/hello2.lua`).

`hello2.lua:`
```lua
world = ' World'

function main()
  while true do
    print('Hello' .. world)
  end
end

live.patch('main', main)

live.start(main)
```

The program should start outputing `Hello World`, after all buffers clear.
