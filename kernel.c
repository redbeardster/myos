// kernel.c
void kmain(unsigned int magic, unsigned int addr) {
    // Указатель на видеопамять (текстовый режим 80x25, цветной)
    char* video_memory = (char*) 0xB8000;
    const char* message = "Hello, my first OS kernel!";

    // Очищаем экран (заполняем черным фоном и черным текстом)
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        video_memory[i] = ' ';        // Символ пробела
        video_memory[i + 1] = 0x00;   // Черный на черном
    }

    // Выводим сообщение (белый текст на синем фоне)
    // Атрибут: 0x1F = Белый текст (F) на синем фоне (1)
    int i = 0;
    while (message[i] != '\0') {
        video_memory[i * 2] = message[i];
        video_memory[i * 2 + 1] = 0x1F; // Белый на синем
        i++;
    }

    // Бесконечный цикл, чтобы ядро не завершилось
    while (1) {
        // Можно добавить паузу, но Qemu и так симулирует работу
    }
}
