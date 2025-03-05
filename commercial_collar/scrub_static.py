bad_words = [
    '/Users/stevereed/Code/Culvert/purina/modules', 
    '/Users/stevereed/Code/Culvert/purina/zephyr',
    '/Users/stevereed/Code/Culvert/purina/nrf',
    'macro name is a reserved identifier',
    'is reserved because it starts with',
    'unused parameter',
    '/Users/stevereed/Code/Culvert/purina/bootloader',
    '/opt/nordic/ncs/toolchains/20d68df7e5/opt/zephyr-sdk',
    'clang-diagnostic-switch-default'
    ]

other_skips = [
    'defects in',
    'Found no defects in',
    'defect(s) in'
]

with open('statictest.txt') as oldfile, open('newstatic.txt', 'w') as newfile:
    for line in oldfile:
        if any(bad_word in line for bad_word in bad_words):
            # skip this line and the next 3 lines
            for _ in range(3):
                next(oldfile)
        else:
            if any(bad_word in line for bad_word in other_skips):
                next(oldfile)
            else:
                newfile.write(line)
            
