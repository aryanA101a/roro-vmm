## UART 16550

```

uint8 {
    RBR
    THR,
    IER,
    IIR,
    LCR,
    MCR,
    LSR,
    MSR,
    SCR,
    DLL,
    DLM,
}

DR=0x01
THRE=0x20
TEMT=0x40

IIR=0x1
LSR=THRE|TEMT


uart_tx(inp):
    print(inp)
    
uart_rx(byte):
    RBR = byte;
    LSR |= DR;  

write(port, value):
    off=port-0x3f8
    dlab=LCR&0x80

    case(off):

        0:
            if dlab:
                DLL=value
            else:
                THR=value
                uart_tx(THR)

        1:
            if dlab:
                DLM=value
            else:
                IER=value

        2:
            if (value & 0x02) {
                LSR&=~DR
            }

        3:
            LCR=value

        4:
            MCR=value
        
        7:
            SCR=value

read(port):
    off=port-0x3f8
    dlab=LCR&0x80
    ret=0

    case(off):

        0:
            if dlab:
                ret=DLL
            else:
                ret=RBR
                LSR&=~DR


        1:
            if dlab:
                ret=DLM
            else:
                ret=IER

        2:
            ret=IIR

        3:
            ret=LCR

        4:
            ret=MCR
        
        5:
            ret=LSR

        6:
            ret=MSR
        
        7:
            ret=SCR
    
    return ret



```