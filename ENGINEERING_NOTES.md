This will be used to log everything I found difficult, debugged through, and lessons learned. A different approach from my previous projects

Just learned a new C keyword: goto
It is an unconditional jump to a label in the same function. No stack frame, no return, no arguments, nothing pushed or popped. The CPU just branches like it does in assembly I guess.
Example Syntax use:
```C
    goto cleanup;
    cleanup:
        Do_something();
```
			
Fully standard C, and valid forever. Driver code is disciplined and forward-only which means it is highly recommended to use it in kernel C
Some constraints: We use this within the SAME function. They have a function scope. Visible everywhere in the function regardless of block nesting. REMEMBER: you can jump past variable declarations - so do it before using goto. 

While writing the goto ladder, I made the mistake of declaring a variable AFTER one of the goto statement which we need to avoid. So, we need to ALWAYS declare variables at the top of the scope, be it the top of the function for local scope or top of the file for global scope. Furthermore, when we reach the goto statement, it jumps to wherever we have the 'cleanup:' statement. Then, it does not go back to where the goto statement was initially called. It keeps moving from the line AFTER the jump

