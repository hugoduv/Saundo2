$ErrorActionPreference = 'Stop'

try {
    Add-Type -AssemblyName System.Speech
    $culture = [System.Globalization.CultureInfo]::GetCultureInfo('fr-FR')
    $recognizer = New-Object System.Speech.Recognition.SpeechRecognitionEngine $culture
    $builder = New-Object System.Speech.Recognition.GrammarBuilder
    $builder.Culture = $culture
    $builder.Append('stop')
    $recognizer.LoadGrammar((New-Object System.Speech.Recognition.Grammar $builder))
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono
    )
    [Console]::Out.WriteLine('READY')
    [Console]::Out.Flush()
    $inputStream = [Console]::OpenStandardInput()
    $window = New-Object byte[] 64000
    $hasPrevious = $false
    while ($true) {
        $offset = 32000
        while ($offset -lt $window.Length) {
            $read = $inputStream.Read($window, $offset, $window.Length - $offset)
            if ($read -eq 0) { exit 0 }
            $offset += $read
        }
        if ($hasPrevious) {
            $audio = New-Object System.IO.MemoryStream(,$window)
            $recognizer.SetInputToAudioStream($audio, $format)
            do {
                $result = $recognizer.Recognize()
                if ($null -ne $result -and $result.Text -eq 'stop' -and $result.Confidence -ge 0.65) {
                    [Console]::Out.WriteLine('STOP')
                    [Console]::Out.Flush()
                    while ($inputStream.Read($window, 0, $window.Length) -gt 0) { }
                    exit 0
                }
            } while ($null -ne $result)
            $audio.Dispose()
        }
        [Array]::Copy($window, 32000, $window, 0, 32000)
        $hasPrevious = $true
    }
} catch {
    [Console]::Error.WriteLine($_.ToString())
    exit 1
}
