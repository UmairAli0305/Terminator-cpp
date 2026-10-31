#!/bin/bash
X=10

if [ $X -gt 5 ]; then
    echo Outer condition TRUE

    if [ $X -eq 10 ]; then
        echo Nested condition TRUE
    else
        echo Nested condition FALSE
    fi

else
    echo Outer condition FALSE
fi

echo Script completed
