#include <reg52.h>
#include <intrins.h>
#include <stdio.h>

/* 硬件引脚定义 */
#define LCD_DATA P0
sbit LCD_RS = P2^6;
sbit LCD_RW = P2^5;
sbit LCD_EN = P2^7;
sbit DQ     = P3^7;
sbit LED_RED  = P3^0;
sbit LED_BLUE = P3^1;
sbit BEEP     = P3^2;
sbit KEY_PAUSE= P3^3;
sbit OUT_P17  = P1^7;
sbit KEY_UP   = P2^3;
sbit KEY_DOWN = P2^4;

/* 温度设定与运行状态 */
float TargetTemp = 25.0;
float ActualTemp = 25.0;  /* 传感器读取失败时保留上一次有效温度 */
volatile unsigned int CoolingSeconds = 0;

bit isAlarm = 0;
bit isPaused = 0;

/* 延时函数：DS18B20 时序操作期间会临时关闭总中断 */
void Delay1ms(unsigned int ms) {
    unsigned char i, j;
    while(ms--) { _nop_(); i = 2; j = 199; do { while (--j); } while (--i); }
}
void Delay10us(unsigned int us) { while(us--); }

/* 复位 DS18B20 并读取其应答信号；返回 0 表示检测到设备 */
bit DS18B20_Init() {
    bit ack; bit ea_save = EA; EA = 0;
    DQ = 1; _nop_(); DQ = 0; Delay10us(80);
    DQ = 1; Delay10us(10); ack = DQ; Delay10us(40);
    EA = ea_save; return ack;
}

/* 向 DS18B20 按低位优先写入一个字节 */
void DS18B20_WriteByte(unsigned char dat) {
    unsigned char i; bit ea_save = EA; EA = 0;
    for (i=0; i<8; i++) { DQ = 0; _nop_(); DQ = dat & 0x01; Delay10us(5); DQ = 1; dat >>= 1; }
    EA = ea_save;
}

/* 从 DS18B20 按低位优先读取一个字节 */
unsigned char DS18B20_ReadByte() {
    unsigned char i, dat = 0; bit ea_save = EA; EA = 0;
    for (i=0; i<8; i++) { DQ = 0; _nop_(); DQ = 1; _nop_(); if(DQ) dat |= (0x01 << i); Delay10us(5); }
    EA = ea_save; return dat;
}

/* 读取温度，过滤无效值与 DS18B20 上电默认值 */
float Get_Temp_Filtered() {
    unsigned char LSB, MSB;
    int temp;
    float newTemp;

    if(DS18B20_Init()) return ActualTemp;  /* 未检测到传感器时保持原值 */
    DS18B20_WriteByte(0xCC);
    DS18B20_WriteByte(0x44);

    if(DS18B20_Init()) return ActualTemp;
    DS18B20_WriteByte(0xCC);
    DS18B20_WriteByte(0xBE);

    LSB = DS18B20_ReadByte();
    MSB = DS18B20_ReadByte();

    temp = (MSB << 8) | LSB;
    newTemp = temp * 0.0625;

    /* 0℃和高于 85℃的数据视为本次无效读数 */
    if (newTemp == 0.00) return ActualTemp;
    if (newTemp > 85.0)  return ActualTemp;

    return newTemp;
}

/* 定时器 0 每 500 微秒中断一次，用于蜂鸣器和计时 */
void Timer0_Init() {
    TMOD &= 0xF0; TMOD |= 0x01;
    TH0 = 0xFE; TL0 = 0x33;  /* 500 微秒 */
    ET0 = 1; EA = 1; TR0 = 1;
}

/* 告警时翻转蜂鸣器，并累计制冷运行时间 */
void Timer0_ISR() interrupt 1 {
    static unsigned int ms_count = 0;
    TH0 = 0xFE; TL0 = 0x33;
    if (isAlarm == 1 && isPaused == 0) { BEEP = ~BEEP; }
    else { BEEP = 1; }
    if (isAlarm == 1 && isPaused == 0) {
        ms_count++;
        if (ms_count >= 2000) { ms_count = 0; CoolingSeconds++; }
    }
}

/* 扫描暂停、升温和降温按键，并进行简单消抖 */
void Key_Scan() {
    if (KEY_PAUSE == 0) {
        Delay1ms(10);
        if (KEY_PAUSE == 0) { isPaused = !isPaused; if(isPaused) BEEP = 1; while(KEY_PAUSE == 0); }
    }
    if (!isPaused) {
        if (KEY_UP == 0) { Delay1ms(10); if (KEY_UP == 0) { TargetTemp += 0.5; while(KEY_UP == 0); } }
        if (KEY_DOWN == 0) { Delay1ms(10); if (KEY_DOWN == 0) { TargetTemp -= 0.5; while(KEY_DOWN == 0); } }
    }
}

/* 根据设定温度和 0.5℃回差控制告警、指示灯与输出端 */
void Control_Logic() {
    if (isPaused) { LED_RED = 1; LED_BLUE = 1; OUT_P17 = 0; return; }

    if (ActualTemp >= (TargetTemp + 0.5)) {
        isAlarm = 1; LED_BLUE = 0; LED_RED = 1; OUT_P17 = 1;
    }
    else if (ActualTemp <= (TargetTemp - 0.5)) {
        isAlarm = 0; LED_BLUE = 1; LED_RED = 0; OUT_P17 = 0; CoolingSeconds = 0;
    }
}

/* LCD1602 基本读写与初始化 */
void LCD_WriteCmd(unsigned char cmd) { LCD_RS = 0; LCD_RW = 0; LCD_EN = 0; LCD_DATA = cmd; Delay1ms(1); LCD_EN = 1; Delay1ms(1); LCD_EN = 0; }
void LCD_WriteData(unsigned char dat) { LCD_RS = 1; LCD_RW = 0; LCD_EN = 0; LCD_DATA = dat; Delay1ms(1); LCD_EN = 1; Delay1ms(1); LCD_EN = 0; }
void LCD_Init() { LCD_WriteCmd(0x38); LCD_WriteCmd(0x0C); LCD_WriteCmd(0x06); LCD_WriteCmd(0x01); }

/* 在 LCD 指定行列显示字符串 */
void LCD_ShowString(unsigned char x, unsigned char y, char *str) {
    if (y == 0) LCD_WriteCmd(0x80 + x); else LCD_WriteCmd(0xC0 + x);
    while (*str) { LCD_WriteData(*str++); }
}

/* 刷新设定温度、当前温度、制冷时间及暂停状态 */
void Display_Update() {
    char buf[17];
    unsigned int m, s;
    m = CoolingSeconds / 60;
    s = CoolingSeconds % 60;

    sprintf(buf, "Set:%.1f C      ", TargetTemp);
    if(isPaused) buf[15] = 'P';
    LCD_ShowString(0, 0, buf);

    sprintf(buf, "C:%.1f T:%02u:%02u  ", ActualTemp, m, s);
    LCD_ShowString(0, 1, buf);
}

/* 初始化外设后循环执行温度采集、控制和显示 */
void main() {
    LCD_Init();
    Timer0_Init();
    LED_RED = 1; OUT_P17 = 1; LED_BLUE = 1; BEEP = 1;

    while(1) {
        ActualTemp = Get_Temp_Filtered();
        Key_Scan();
        Control_Logic();
        Display_Update();
        Delay1ms(10);
    }
}