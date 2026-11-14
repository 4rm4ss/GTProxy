GTProxy для Windows

1. Файл hosts. Откройте Блокнот от имени администратора, затем
   C:\Windows\System32\drivers\etc\hosts и добавьте в конец:

   127.0.0.1 www.growtopia1.com
   127.0.0.1 www.growtopia2.com

2. Один раз запустите install-cert.bat (двойной щелчок).
   Прокси создаст свой сертификат в папке resources и добавит его
   в доверенные для вашей учётной записи. Windows покажет окно
   с вопросом - нажмите "Да". Без этого игра не подключится.

3. Запустите Proxy.exe, затем игру.
   Если Windows пишет, что не найден VCRUNTIME140.dll или MSVCP140.dll,
   установите "Microsoft Visual C++ Redistributable" (x64) с сайта Microsoft.
   Порт 443 должен быть свободен (его иногда занимают Skype, IIS, VMware).

Если что-то не работает, пришлите файл proxy.log из этой папки.

Чтобы играть без прокси, уберите строки из hosts.
Удалить сертификат: uninstall-cert.bat.
Никому не передавайте файлы resources\*.key.pem - это ваши закрытые ключи.
