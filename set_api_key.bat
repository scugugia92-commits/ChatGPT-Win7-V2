@echo off
set /p OPENAIKEY=Incolla la tua OpenAI API key: 
setx OPENAI_API_KEY "%OPENAIKEY%"
echo.
echo Chiudi e riapri ChatGPT-Win7 dopo questo comando.
pause
