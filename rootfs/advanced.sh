set NAME Bharat

function greet
echo Hello $NAME
end

loop 3
greet
endloop

if EXIST test.txt
echo file exists >> log.txt
else
echo missing >> log.txt
endif
